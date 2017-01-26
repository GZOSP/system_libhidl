/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ServiceManagement"

#include <hidl/HidlBinderSupport.h>
#include <hidl/ServiceManagement.h>
#include <hidl/Static.h>
#include <hidl/Status.h>

#include <android-base/logging.h>
#include <condition_variable>
#include <dlfcn.h>
#include <dirent.h>
#include <hidl-util/FQName.h>
#include <hidl-util/StringHelper.h>
#include <hwbinder/IPCThreadState.h>
#include <hwbinder/Parcel.h>
#include <mutex>
#include <unistd.h>

#include <android/hidl/manager/1.0/IServiceManager.h>
#include <android/hidl/manager/1.0/BpHwServiceManager.h>
#include <android/hidl/manager/1.0/BnHwServiceManager.h>

using android::hidl::manager::V1_0::IServiceManager;
using android::hidl::manager::V1_0::IServiceNotification;
using android::hidl::manager::V1_0::BpHwServiceManager;
using android::hidl::manager::V1_0::BnHwServiceManager;

namespace android {
namespace hardware {

sp<IServiceManager> defaultServiceManager() {

    if (gDefaultServiceManager != NULL) return gDefaultServiceManager;
    if (access("/dev/hwbinder", F_OK|R_OK|W_OK) != 0) {
        // HwBinder not available on this device or not accessible to
        // this process.
        return nullptr;
    }
    {
        AutoMutex _l(gDefaultServiceManagerLock);
        while (gDefaultServiceManager == NULL) {
            gDefaultServiceManager = fromBinder<IServiceManager, BpHwServiceManager, BnHwServiceManager>(
                ProcessState::self()->getContextObject(NULL));
            if (gDefaultServiceManager == NULL)
                sleep(1);
        }
    }

    return gDefaultServiceManager;
}

std::vector<std::string> search(const std::string &path,
                              const std::string &prefix,
                              const std::string &suffix) {
    std::unique_ptr<DIR, decltype(&closedir)> dir(opendir(path.c_str()), closedir);
    if (!dir) return {};

    std::vector<std::string> results{};

    dirent* dp;
    while ((dp = readdir(dir.get())) != nullptr) {
        std::string name = dp->d_name;

        if (StringHelper::StartsWith(name, prefix) &&
                StringHelper::EndsWith(name, suffix)) {
            results.push_back(name);
        }
    }

    return results;
}

struct PassthroughServiceManager : IServiceManager {
    Return<sp<IBase>> get(const hidl_string& fqName,
                     const hidl_string& name) override {
        FQName iface(fqName);

        if (!iface.isValid() ||
            !iface.isFullyQualified() ||
            iface.isIdentifier()) {
            LOG(ERROR) << "Invalid interface name passthrough lookup: " << fqName;
            return nullptr;
        }

        const int dlMode = RTLD_LAZY;
        void *handle = nullptr;

        std::string library;

        // TODO: lookup in VINTF instead
        // TODO(b/34135607): Remove HAL_LIBRARY_PATH_SYSTEM

        for (const std::string &path : {
            HAL_LIBRARY_PATH_ODM, HAL_LIBRARY_PATH_VENDOR, HAL_LIBRARY_PATH_SYSTEM
        }) {
            const std::string prefix = iface.getPackageAndVersion().string() + "-impl";

            std::vector<std::string> libs = search(path, prefix, ".so");

            if (libs.size() > 1) {
                LOG(WARNING) << "Multiple libraries found: " << StringHelper::JoinStrings(libs, ", ");
            }

            for (const std::string &lib : libs) {
                handle = dlopen((path + lib).c_str(), dlMode);
                if (handle != nullptr) {
                    library = lib;
                    goto beginLookup;
                }
            }
        }

        if (handle == nullptr) {
            return nullptr;
        }
beginLookup:

        const std::string sym = "HIDL_FETCH_" + iface.name();

        IBase* (*generator)(const char* name);
        *(void **)(&generator) = dlsym(handle, sym.c_str());
        if(!generator) {
            LOG(ERROR) << "Passthrough lookup opened " << library
                       << " but could not find symbol " << sym;
            return nullptr;
        }
        return (*generator)(name);
    }

    Return<bool> add(const hidl_vec<hidl_string>& /* interfaceChain */,
                     const hidl_string& /* name */,
                     const sp<IBase>& /* service */) override {
        LOG(FATAL) << "Cannot register services with passthrough service manager.";
        return false;
    }

    Return<void> list(list_cb /* _hidl_cb */) override {
        // TODO: add this functionality
        LOG(FATAL) << "Cannot list services with passthrough service manager.";
        return Void();
    }
    Return<void> listByInterface(const hidl_string& /* fqInstanceName */,
                                 listByInterface_cb /* _hidl_cb */) override {
        // TODO: add this functionality
        LOG(FATAL) << "Cannot list services with passthrough service manager.";
        return Void();
    }

    Return<bool> registerForNotifications(const hidl_string& /* fqName */,
                                          const hidl_string& /* name */,
                                          const sp<IServiceNotification>& /* callback */) override {
        // This makes no sense.
        LOG(FATAL) << "Cannot register for notifications with passthrough service manager.";
        return false;
    }

};

sp<IServiceManager> getPassthroughServiceManager() {
    static sp<PassthroughServiceManager> manager(new PassthroughServiceManager());
    return manager;
}

namespace details {

struct Waiter : IServiceNotification {
    Return<void> onRegistration(const hidl_string& /* fqName */,
                                const hidl_string& /* name */,
                                bool /* preexisting */) override {
        std::unique_lock<std::mutex> lock(mMutex);
        if (mRegistered) {
            return Void();
        }
        mRegistered = true;
        lock.unlock();

        mCondition.notify_one();
        return Void();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mMutex);
        mCondition.wait(lock, [this]{
            return mRegistered;
        });
    }

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    bool mRegistered = false;
};

void waitForHwService(
        const std::string &interface, const std::string &instanceName) {
    const sp<IServiceManager> manager = defaultServiceManager();

    if (manager == nullptr) {
        LOG(ERROR) << "Could not get default service manager.";
        return;
    }

    sp<Waiter> waiter = new Waiter();
    Return<bool> ret = manager->registerForNotifications(interface, instanceName, waiter);

    if (!ret.isOk()) {
        LOG(ERROR) << "Transport error, " << ret.description()
            << ", during notification registration for "
            << interface << "/" << instanceName << ".";
        return;
    }

    if (!ret) {
        LOG(ERROR) << "Could not register for notifications for "
            << interface << "/" << instanceName << ".";
        return;
    }

    waiter->wait();
}

}; // namespace details

}; // namespace hardware
}; // namespace android
