/* -*- Mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; tab-width: 40 -*- */
/* vim: set ts=2 et sw=2 tw=40: */
/*
** Copyright 2006, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include "base/basictypes.h"
#include "BluetoothDBusService.h"
#include "BluetoothTypes.h"
#include "BluetoothReplyRunnable.h"

#include <cstdio>
#include <dbus/dbus.h>

#include "nsIDOMDOMRequest.h"
#include "nsAutoPtr.h"
#include "nsThreadUtils.h"
#include "nsDebug.h"
#include "nsClassHashtable.h"
#include "mozilla/ipc/DBusThread.h"
#include "mozilla/ipc/DBusUtils.h"
#include "mozilla/ipc/RawDBusConnection.h"
#include "mozilla/Util.h"

/**
 * Some rules for dealing with memory in DBus:
 * - A DBusError only needs to be deleted if it's been set, not just
 *   initialized. This is why LOG_AND_FREE... is called only when an error is
 *   set, and the macro cleans up the error itself.
 * - A DBusMessage needs to be unrefed when it is newed explicitly. DBusMessages
 *   from signals do not need to be unrefed, as they will be cleaned by DBus
 *   after DBUS_HANDLER_RESULT_HANDLED is returned from the filter.
 */

using namespace mozilla;
using namespace mozilla::ipc;
USING_BLUETOOTH_NAMESPACE

#undef LOG
#if defined(MOZ_WIDGET_GONK)
#include <android/log.h>
#define LOG(args...)  __android_log_print(ANDROID_LOG_INFO, "GonkDBus", args);
#else
#define BTDEBUG true
#define LOG(args...) if (BTDEBUG) printf(args);
#endif

#define DBUS_MANAGER_IFACE BLUEZ_DBUS_BASE_IFC ".Manager"
#define DBUS_ADAPTER_IFACE BLUEZ_DBUS_BASE_IFC ".Adapter"
#define DBUS_DEVICE_IFACE BLUEZ_DBUS_BASE_IFC ".Device"
#define BLUEZ_DBUS_BASE_PATH      "/org/bluez"
#define BLUEZ_DBUS_BASE_IFC       "org.bluez"
#define BLUEZ_ERROR_IFC           "org.bluez.Error"

typedef struct {
  const char* name;
  int type;
} Properties;

static Properties sDeviceProperties[] = {
  {"Address", DBUS_TYPE_STRING},
  {"Name", DBUS_TYPE_STRING},
  {"Icon", DBUS_TYPE_STRING},
  {"Class", DBUS_TYPE_UINT32},
  {"UUIDs", DBUS_TYPE_ARRAY},
  {"Services", DBUS_TYPE_ARRAY},
  {"Paired", DBUS_TYPE_BOOLEAN},
  {"Connected", DBUS_TYPE_BOOLEAN},
  {"Trusted", DBUS_TYPE_BOOLEAN},
  {"Blocked", DBUS_TYPE_BOOLEAN},
  {"Alias", DBUS_TYPE_STRING},
  {"Nodes", DBUS_TYPE_ARRAY},
  {"Adapter", DBUS_TYPE_OBJECT_PATH},
  {"LegacyPairing", DBUS_TYPE_BOOLEAN},
  {"RSSI", DBUS_TYPE_INT16},
  {"TX", DBUS_TYPE_UINT32},
  {"Broadcaster", DBUS_TYPE_BOOLEAN}
};

static Properties sAdapterProperties[] = {
  {"Address", DBUS_TYPE_STRING},
  {"Name", DBUS_TYPE_STRING},
  {"Class", DBUS_TYPE_UINT32},
  {"Powered", DBUS_TYPE_BOOLEAN},
  {"Discoverable", DBUS_TYPE_BOOLEAN},
  {"DiscoverableTimeout", DBUS_TYPE_UINT32},
  {"Pairable", DBUS_TYPE_BOOLEAN},
  {"PairableTimeout", DBUS_TYPE_UINT32},
  {"Discovering", DBUS_TYPE_BOOLEAN},
  {"Devices", DBUS_TYPE_ARRAY},
  {"UUIDs", DBUS_TYPE_ARRAY},
};

static Properties sManagerProperties[] = {
  {"Adapters", DBUS_TYPE_ARRAY},
};

static const char* sBluetoothDBusIfaces[] =
{
  DBUS_MANAGER_IFACE,
  DBUS_ADAPTER_IFACE,
  DBUS_DEVICE_IFACE
};

static const char* sBluetoothDBusSignals[] =
{
  "type='signal',interface='org.freedesktop.DBus'",
  "type='signal',interface='org.bluez.Adapter'",
  "type='signal',interface='org.bluez.Manager'",
  "type='signal',interface='org.bluez.Device'",
  "type='signal',interface='org.bluez.Input'",
  "type='signal',interface='org.bluez.Network'",
  "type='signal',interface='org.bluez.NetworkServer'",
  "type='signal',interface='org.bluez.HealthDevice'",
  "type='signal',interface='org.bluez.AudioSink'"
};

/**
 * DBus Connection held for the BluetoothCommandThread to use. Should never be
 * used by any other thread.
 * 
 */
static nsAutoPtr<RawDBusConnection> gThreadConnection;

class DistributeBluetoothSignalTask : public nsRunnable {
  BluetoothSignal mSignal;
public:
  DistributeBluetoothSignalTask(const BluetoothSignal& aSignal) :
    mSignal(aSignal)
  {
  }

  NS_IMETHOD
  Run()
  {
    MOZ_ASSERT(NS_IsMainThread());
    BluetoothService* bs = BluetoothService::Get();
    if (!bs) {
      NS_WARNING("BluetoothService not available!");
      return NS_ERROR_FAILURE;
    }    
    return bs->DistributeSignal(mSignal);
  }  
};

bool
IsDBusMessageError(DBusMessage* aMsg, DBusError* aErr, nsAString& aErrorStr)
{
  if(aErr && dbus_error_is_set(aErr)) {
    aErrorStr = NS_ConvertUTF8toUTF16(aErr->message);
    LOG_AND_FREE_DBUS_ERROR(aErr);
    return true;
  }
  
  DBusError err;
  dbus_error_init(&err);
  if (dbus_message_get_type(aMsg) == DBUS_MESSAGE_TYPE_ERROR) {
    const char* error_msg;
    if (!dbus_message_get_args(aMsg, &err, DBUS_TYPE_STRING,
                               &error_msg, DBUS_TYPE_INVALID) ||
        !error_msg) {
      if (dbus_error_is_set(&err)) {
        aErrorStr = NS_ConvertUTF8toUTF16(err.message);
        LOG_AND_FREE_DBUS_ERROR(&err);
        return true;
      } else {
        aErrorStr.AssignLiteral("Unknown Error");
        return true;
      }
    } else {
      aErrorStr = NS_ConvertUTF8toUTF16(error_msg);
      return true;
    }
  }
  return false;
}

void
DispatchBluetoothReply(BluetoothReplyRunnable* aRunnable,
                       const BluetoothValue& aValue, const nsAString& aErrorStr)
{
  // Reply will be deleted by the runnable after running on main thread
  BluetoothReply* reply;
  if (!aErrorStr.IsEmpty()) {
    nsString err(aErrorStr);
    reply = new BluetoothReply(BluetoothReplyError(err));
  } else {
    reply = new BluetoothReply(BluetoothReplySuccess(aValue));
  }
  
  aRunnable->SetReply(reply);
  if (NS_FAILED(NS_DispatchToMainThread(aRunnable))) {
    NS_WARNING("Failed to dispatch to main thread!");
  }
}

void
UnpackObjectPathMessage(DBusMessage* aMsg, DBusError* aErr,
                        BluetoothValue& aValue, nsAString& aErrorStr)
{
  DBusError err;
  dbus_error_init(&err);
  if (!IsDBusMessageError(aMsg, aErr, aErrorStr)) {
    NS_ASSERTION(dbus_message_get_type(aMsg) == DBUS_MESSAGE_TYPE_METHOD_RETURN,
                 "Got dbus callback that's not a METHOD_RETURN!");
    const char* object_path;
    if (!dbus_message_get_args(aMsg, &err, DBUS_TYPE_OBJECT_PATH,
                               &object_path, DBUS_TYPE_INVALID) ||
        !object_path) {
      if (dbus_error_is_set(&err)) {
        aErrorStr = NS_ConvertUTF8toUTF16(err.message);
        LOG_AND_FREE_DBUS_ERROR(&err);
      }
    } else {
      aValue = NS_ConvertUTF8toUTF16(object_path);
    }
  }
}

typedef void (*UnpackFunc)(DBusMessage*, DBusError*, BluetoothValue&, nsAString&);

void
RunDBusCallback(DBusMessage* aMsg, void* aBluetoothReplyRunnable,
                UnpackFunc aFunc)
{
  MOZ_ASSERT(!NS_IsMainThread());
  nsRefPtr<BluetoothReplyRunnable> replyRunnable =
    dont_AddRef(static_cast< BluetoothReplyRunnable* >(aBluetoothReplyRunnable));

  NS_ASSERTION(replyRunnable, "Callback reply runnable is null!");

  nsString replyError;
  BluetoothValue v;
  aFunc(aMsg, nullptr, v, replyError);
  DispatchBluetoothReply(replyRunnable, v, replyError);  
}

void
GetObjectPathCallback(DBusMessage* aMsg, void* aBluetoothReplyRunnable)
{
  RunDBusCallback(aMsg, aBluetoothReplyRunnable,
                  UnpackObjectPathMessage);
}

void
UnpackVoidMessage(DBusMessage* aMsg, DBusError* aErr, BluetoothValue& aValue,
                  nsAString& aErrorStr)
{
  DBusError err;
  dbus_error_init(&err);
  if (!IsDBusMessageError(aMsg, aErr, aErrorStr) &&
      dbus_message_get_type(aMsg) == DBUS_MESSAGE_TYPE_METHOD_RETURN &&
      !dbus_message_get_args(aMsg, &err, DBUS_TYPE_INVALID)) {
    if (dbus_error_is_set(&err)) {
      aErrorStr = NS_ConvertUTF8toUTF16(err.message);
      LOG_AND_FREE_DBUS_ERROR(&err);
    }
  }
}

void
GetVoidCallback(DBusMessage* aMsg, void* aBluetoothReplyRunnable)
{
  RunDBusCallback(aMsg, aBluetoothReplyRunnable,
                  UnpackVoidMessage);
}

bool
GetProperty(DBusMessageIter aIter, Properties* aPropertyTypes,
            int aPropertyTypeLen, int* aPropIndex,
            InfallibleTArray<BluetoothNamedValue>& aProperties)
{
  DBusMessageIter prop_val, array_val_iter;
  char* property = NULL;
  uint32_t array_type;
  int i, type;

  if (dbus_message_iter_get_arg_type(&aIter) != DBUS_TYPE_STRING) {    
    return false;
  }

  dbus_message_iter_get_basic(&aIter, &property);

  if (!dbus_message_iter_next(&aIter) ||
      dbus_message_iter_get_arg_type(&aIter) != DBUS_TYPE_VARIANT) {
    return false;
  }

  for (i = 0; i < aPropertyTypeLen; i++) {
    if (!strncmp(property, aPropertyTypes[i].name, strlen(property))) {      
      break;
    }
  }

  if (i == aPropertyTypeLen) {
    return false;
  }

  nsString propertyName;
  propertyName.AssignASCII(aPropertyTypes[i].name);
  *aPropIndex = i;

  dbus_message_iter_recurse(&aIter, &prop_val);
  type = aPropertyTypes[*aPropIndex].type;

  NS_ASSERTION(dbus_message_iter_get_arg_type(&prop_val) == type,
               "Iterator not type we expect!");
  
  BluetoothValue propertyValue;
  switch (type) {
    case DBUS_TYPE_STRING:
    case DBUS_TYPE_OBJECT_PATH:
      const char* c;
      dbus_message_iter_get_basic(&prop_val, &c);
      propertyValue = NS_ConvertUTF8toUTF16(c);
      break;
    case DBUS_TYPE_UINT32:
    case DBUS_TYPE_INT16:
      uint32_t i;
      dbus_message_iter_get_basic(&prop_val, &i);
      propertyValue = i;
      break;
    case DBUS_TYPE_BOOLEAN:
      bool b;
      dbus_message_iter_get_basic(&prop_val, &b);
      propertyValue = b;
      break;
    case DBUS_TYPE_ARRAY:
      dbus_message_iter_recurse(&prop_val, &array_val_iter);
      array_type = dbus_message_iter_get_arg_type(&array_val_iter);
      if (array_type == DBUS_TYPE_OBJECT_PATH ||
          array_type == DBUS_TYPE_STRING){
        InfallibleTArray<nsString> arr;
        do {
          const char* tmp;
          dbus_message_iter_get_basic(&array_val_iter, &tmp);
          nsString s;
          s = NS_ConvertUTF8toUTF16(tmp);
          arr.AppendElement(s);
        } while (dbus_message_iter_next(&array_val_iter));
        propertyValue = arr;
      } else {
        // This happens when the array is 0-length. Apparently we get a
        // DBUS_TYPE_INVALID type.
        propertyValue = InfallibleTArray<nsString>();
#ifdef DEBUG
        NS_WARNING("Received array type that's not a string array!");
#endif
      }
      break;
    default:
      NS_NOTREACHED("Cannot find dbus message type!");
  }
  aProperties.AppendElement(BluetoothNamedValue(propertyName, propertyValue));
  return true;
}

void 
ParseProperties(DBusMessageIter* aIter,
                BluetoothValue& aValue,
                nsAString& aErrorStr,
                Properties* aPropertyTypes,
                const int aPropertyTypeLen)
{
  DBusMessageIter dict_entry, dict;
  int prop_index = -1;

  NS_ASSERTION(dbus_message_iter_get_arg_type(aIter) == DBUS_TYPE_ARRAY,
               "Trying to parse a property from something that's not an array!");

  dbus_message_iter_recurse(aIter, &dict);
  InfallibleTArray<BluetoothNamedValue> props;
  do {
    NS_ASSERTION(dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY,
                 "Trying to parse a property from something that's not an dict!");
    dbus_message_iter_recurse(&dict, &dict_entry);

    if (!GetProperty(dict_entry, aPropertyTypes, aPropertyTypeLen, &prop_index,
                     props)) {
      aErrorStr.AssignLiteral("Can't Create Property!");
      NS_WARNING("Can't create property!");
      return;
    }
  } while (dbus_message_iter_next(&dict));

  aValue = props;
}

void
UnpackPropertiesMessage(DBusMessage* aMsg, DBusError* aErr,
                        BluetoothValue& aValue, nsAString& aErrorStr,
                        Properties* aPropertyTypes,
                        const int aPropertyTypeLen)
{
  if (!IsDBusMessageError(aMsg, aErr, aErrorStr) &&
      dbus_message_get_type(aMsg) == DBUS_MESSAGE_TYPE_METHOD_RETURN) {
    DBusMessageIter iter;
    if (!dbus_message_iter_init(aMsg, &iter)) {
      aErrorStr.AssignLiteral("Cannot create dbus message iter!");
    } else {
      ParseProperties(&iter, aValue, aErrorStr, aPropertyTypes,
                      aPropertyTypeLen);
    }
  }
}

void
UnpackAdapterPropertiesMessage(DBusMessage* aMsg, DBusError* aErr,
                               BluetoothValue& aValue,
                               nsAString& aErrorStr)
{
  UnpackPropertiesMessage(aMsg, aErr, aValue, aErrorStr,
                          sAdapterProperties,
                          ArrayLength(sAdapterProperties));
}

void
UnpackDevicePropertiesMessage(DBusMessage* aMsg, DBusError* aErr,
                              BluetoothValue& aValue,
                              nsAString& aErrorStr)
{
  UnpackPropertiesMessage(aMsg, aErr, aValue, aErrorStr,
                          sDeviceProperties,
                          ArrayLength(sDeviceProperties));
}

void
UnpackManagerPropertiesMessage(DBusMessage* aMsg, DBusError* aErr,
                               BluetoothValue& aValue,
                               nsAString& aErrorStr)
{
  UnpackPropertiesMessage(aMsg, aErr, aValue, aErrorStr,
                          sManagerProperties,
                          ArrayLength(sManagerProperties));
}

void
GetManagerPropertiesCallback(DBusMessage* aMsg, void* aBluetoothReplyRunnable)
{
  RunDBusCallback(aMsg, aBluetoothReplyRunnable,
                  UnpackManagerPropertiesMessage);
}

void
GetAdapterPropertiesCallback(DBusMessage* aMsg, void* aBluetoothReplyRunnable)
{
  RunDBusCallback(aMsg, aBluetoothReplyRunnable,
                  UnpackAdapterPropertiesMessage);
}

void
GetDevicePropertiesCallback(DBusMessage* aMsg, void* aBluetoothReplyRunnable)
{
  RunDBusCallback(aMsg, aBluetoothReplyRunnable,
                  UnpackDevicePropertiesMessage);
}

static DBusCallback sBluetoothDBusPropCallbacks[] =
{
  GetManagerPropertiesCallback,
  GetAdapterPropertiesCallback,
  GetDevicePropertiesCallback
};

MOZ_STATIC_ASSERT(sizeof(sBluetoothDBusPropCallbacks) == sizeof(sBluetoothDBusIfaces),
  "DBus Property callback array and DBus interface array must be same size");

void
ParsePropertyChange(DBusMessage* aMsg, BluetoothValue& aValue,
                    nsAString& aErrorStr, Properties* aPropertyTypes,
                    const int aPropertyTypeLen)
{
  DBusMessageIter iter;
  DBusError err;
  int prop_index = -1;
  InfallibleTArray<BluetoothNamedValue> props;
  
  dbus_error_init(&err);
  if (!dbus_message_iter_init(aMsg, &iter)) {
    LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, aMsg);
    return;
  }
    
  if (!GetProperty(iter, aPropertyTypes, aPropertyTypeLen,
                   &prop_index, props)) {
    NS_WARNING("Can't get property!");
    aErrorStr.AssignLiteral("Can't get property!");
    return;
  }
  aValue = props;
}

// Called by dbus during WaitForAndDispatchEventNative()
// This function is called on the IOThread
static
DBusHandlerResult
EventFilter(DBusConnection* aConn, DBusMessage* aMsg, void* aData)
{
  NS_ASSERTION(!NS_IsMainThread(), "Shouldn't be called from Main Thread!");
  
  if (dbus_message_get_type(aMsg) != DBUS_MESSAGE_TYPE_SIGNAL) {
    LOG("%s: not interested (not a signal).\n", __FUNCTION__);
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }  

  if (dbus_message_get_path(aMsg) == NULL) {
    LOG("DBusMessage %s has no bluetooth destination, ignoring\n",
        dbus_message_get_member(aMsg));
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  DBusError err;
  nsString signalPath;
  nsString signalName;
  dbus_error_init(&err);
  signalPath = NS_ConvertUTF8toUTF16(dbus_message_get_path(aMsg));
  signalName = NS_ConvertUTF8toUTF16(dbus_message_get_member(aMsg));
  nsString errorStr;
  BluetoothValue v;
  
  if (dbus_message_is_signal(aMsg, DBUS_ADAPTER_IFACE, "DeviceFound")) {

    DBusMessageIter iter;

    if (!dbus_message_iter_init(aMsg, &iter)) {
      NS_WARNING("Can't create iterator!");
      return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    }

    const char* addr;
    dbus_message_iter_get_basic(&iter, &addr);
    
    if (dbus_message_iter_next(&iter)) {
      ParseProperties(&iter,
                      v,
                      errorStr,
                      sDeviceProperties,
                      ArrayLength(sDeviceProperties));
      if (v.type() == BluetoothValue::TArrayOfBluetoothNamedValue)
      {
        // The DBus DeviceFound message actually passes back a key value object
        // with the address as the key and the rest of the device properties as
        // a dict value. After we parse out the properties, we need to go back
        // and add the address to the ipdl dict we've created to make sure we
        // have all of the information to correctly build the device.
        v.get_ArrayOfBluetoothNamedValue()
          .AppendElement(BluetoothNamedValue(NS_LITERAL_STRING("Address"),
                                             NS_ConvertUTF8toUTF16(addr)));
      }
    } else {
      errorStr.AssignLiteral("DBus device found message structure not as expected!");
    }
  } else if (dbus_message_is_signal(aMsg, DBUS_ADAPTER_IFACE, "DeviceDisappeared")) {
    const char* str;
    if (!dbus_message_get_args(aMsg, &err,
                               DBUS_TYPE_STRING, &str,
                               DBUS_TYPE_INVALID)) {
      LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, aMsg);
      errorStr.AssignLiteral("Cannot parse device address!");
    }
    v = NS_ConvertUTF8toUTF16(str);
  } else if (dbus_message_is_signal(aMsg, DBUS_ADAPTER_IFACE, "DeviceCreated")) {
    const char* str;
    if (!dbus_message_get_args(aMsg, &err,
                               DBUS_TYPE_OBJECT_PATH, &str,
                               DBUS_TYPE_INVALID)) {
      LOG_AND_FREE_DBUS_ERROR_WITH_MSG(&err, aMsg);
      errorStr.AssignLiteral("Cannot parse device path!");
    }
    v = NS_ConvertUTF8toUTF16(str);
  } else if (dbus_message_is_signal(aMsg, DBUS_ADAPTER_IFACE, "PropertyChanged")) {
    ParsePropertyChange(aMsg,
                        v,
                        errorStr,
                        sAdapterProperties,
                        ArrayLength(sAdapterProperties));
  } else if (dbus_message_is_signal(aMsg, DBUS_DEVICE_IFACE, "PropertyChanged")) {
    ParsePropertyChange(aMsg,
                        v,
                        errorStr,
                        sDeviceProperties,
                        ArrayLength(sDeviceProperties));
  } else if (dbus_message_is_signal(aMsg, DBUS_MANAGER_IFACE, "PropertyChanged")) {
    ParsePropertyChange(aMsg,
                        v,
                        errorStr,
                        sManagerProperties,
                        ArrayLength(sManagerProperties));
  } else {
#ifdef DEBUG
    nsCAutoString signalStr;
    signalStr += dbus_message_get_member(aMsg);
    signalStr += " Signal not handled!";
    NS_WARNING(signalStr.get());
#endif
  }

  if (!errorStr.IsEmpty()) {
    NS_WARNING(NS_ConvertUTF16toUTF8(errorStr).get());
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
  }

  BluetoothSignal signal(signalName, signalPath, v);
  
  nsRefPtr<DistributeBluetoothSignalTask>
    t = new DistributeBluetoothSignalTask(signal);
  if (NS_FAILED(NS_DispatchToMainThread(t))) {
    NS_WARNING("Failed to dispatch to main thread!");
  }

  return DBUS_HANDLER_RESULT_HANDLED;
}

nsresult
BluetoothDBusService::StartInternal()
{
  // This could block. It should never be run on the main thread.
  MOZ_ASSERT(!NS_IsMainThread());
  
  if (!StartDBus()) {
    NS_WARNING("Cannot start DBus thread!");
    return NS_ERROR_FAILURE;
  }
  
  if (mConnection) {
    return NS_OK;
  }

  if (NS_FAILED(EstablishDBusConnection())) {
    NS_WARNING("Cannot start Main Thread DBus connection!");
    StopDBus();
    return NS_ERROR_FAILURE;
  }

  gThreadConnection = new RawDBusConnection();
  
  if (NS_FAILED(gThreadConnection->EstablishDBusConnection())) {
    NS_WARNING("Cannot start Sync Thread DBus connection!");
    StopDBus();
    return NS_ERROR_FAILURE;
  }

  DBusError err;
  dbus_error_init(&err);

  // Set which messages will be processed by this dbus connection.
  // Since we are maintaining a single thread for all the DBus bluez
  // signals we want, register all of them in this thread at startup.
  // The event handler will sort the destinations out as needed.
  for (uint32_t i = 0; i < ArrayLength(sBluetoothDBusSignals); ++i) {
    dbus_bus_add_match(mConnection,
                       sBluetoothDBusSignals[i],
                       &err);
    if (dbus_error_is_set(&err)) {
      LOG_AND_FREE_DBUS_ERROR(&err);
    }
  }

  // Add a filter for all incoming messages_base
  if (!dbus_connection_add_filter(mConnection, EventFilter,
                                  NULL, NULL)) {
    NS_WARNING("Cannot create DBus Event Filter for DBus Thread!");
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult
BluetoothDBusService::StopInternal()
{
  // This could block. It should never be run on the main thread.
  MOZ_ASSERT(!NS_IsMainThread());
  
  if (!mConnection) {
    StopDBus();
    return NS_OK;
  }

  DBusError err;
  dbus_error_init(&err);
  for (uint32_t i = 0; i < ArrayLength(sBluetoothDBusSignals); ++i) {
    dbus_bus_remove_match(mConnection,
                          sBluetoothDBusSignals[i],
                          &err);
    if (dbus_error_is_set(&err)) {
      LOG_AND_FREE_DBUS_ERROR(&err);
    }
  }

  dbus_connection_remove_filter(mConnection, EventFilter, nullptr);
  
  mConnection = nullptr;
  gThreadConnection = nullptr;
  mBluetoothSignalObserverTable.Clear();
  StopDBus();
  return NS_OK;
}

class DefaultAdapterPropertiesRunnable : public nsRunnable
{
public:
  DefaultAdapterPropertiesRunnable(BluetoothReplyRunnable* aRunnable)
    : mRunnable(dont_AddRef(aRunnable))
  {
  }

  nsresult
  Run()
  {
    MOZ_ASSERT(!NS_IsMainThread());
    DBusError err;
    dbus_error_init(&err);
   
    BluetoothValue v;
    nsString replyError;

    DBusMessage* msg = dbus_func_args_timeout(gThreadConnection->GetConnection(),
                                              1000,
                                              &err,
                                              "/",
                                              DBUS_MANAGER_IFACE,
                                              "DefaultAdapter",
                                              DBUS_TYPE_INVALID);
    UnpackObjectPathMessage(msg, &err, v, replyError);
    if(msg) {
      dbus_message_unref(msg);
    }
    if(!replyError.IsEmpty()) {
      DispatchBluetoothReply(mRunnable, v, replyError);
      return NS_ERROR_FAILURE;
    }

    nsString path = v.get_nsString();
    nsCString tmp_path = NS_ConvertUTF16toUTF8(path);
    const char* object_path = tmp_path.get();
   
    v = InfallibleTArray<BluetoothNamedValue>();
    msg = dbus_func_args_timeout(gThreadConnection->GetConnection(),
                                 1000,
                                 &err,
                                 object_path,
                                 "org.bluez.Adapter",
                                 "GetProperties",
                                 DBUS_TYPE_INVALID);
    UnpackAdapterPropertiesMessage(msg, &err, v, replyError);
   
    if(!replyError.IsEmpty()) {
      DispatchBluetoothReply(mRunnable, v, replyError);
      return NS_ERROR_FAILURE;
    }
    if(msg) {
      dbus_message_unref(msg);
    }
    // We have to manually attach the path to the rest of the elements
    v.get_ArrayOfBluetoothNamedValue().AppendElement(BluetoothNamedValue(NS_LITERAL_STRING("Path"),
                                                                         path));
    DispatchBluetoothReply(mRunnable, v, replyError);
   
    return NS_OK;
  }

private:
  nsRefPtr<BluetoothReplyRunnable> mRunnable;
};

nsresult
BluetoothDBusService::GetDefaultAdapterPathInternal(BluetoothReplyRunnable* aRunnable)
{
  if (!mConnection || !gThreadConnection) {
    NS_ERROR("Bluetooth service not started yet!");
    return NS_ERROR_FAILURE;
  }
  NS_ASSERTION(NS_IsMainThread(), "Must be called from main thread!");
  nsRefPtr<BluetoothReplyRunnable> runnable = aRunnable;

  nsRefPtr<nsRunnable> func(new DefaultAdapterPropertiesRunnable(runnable));
  if (NS_FAILED(mBluetoothCommandThread->Dispatch(func, NS_DISPATCH_NORMAL))) {
    NS_WARNING("Cannot dispatch firmware loading task!");
    return NS_ERROR_FAILURE;
  }

  runnable.forget();
  return NS_OK;
}

nsresult
BluetoothDBusService::SendDiscoveryMessage(const nsAString& aAdapterPath,
                                           const char* aMessageName,
                                           BluetoothReplyRunnable* aRunnable)
{
  if (!mConnection) {
    NS_WARNING("Bluetooth service not started yet!");
    return NS_ERROR_FAILURE;
  }
  NS_ASSERTION(NS_IsMainThread(), "Must be called from main thread!");

  nsRefPtr<BluetoothReplyRunnable> runnable = aRunnable;

  NS_ConvertUTF16toUTF8 s(aAdapterPath);
  if (!dbus_func_args_async(mConnection,
                            1000,
                            GetVoidCallback,
                            (void*)aRunnable,
                            s.get(),
                            DBUS_ADAPTER_IFACE,
                            aMessageName,
                            DBUS_TYPE_INVALID)) {
    NS_WARNING("Could not start async function!");
    return NS_ERROR_FAILURE;
  }
  runnable.forget();
  return NS_OK;
}

nsresult
BluetoothDBusService::StopDiscoveryInternal(const nsAString& aAdapterPath,
                                            BluetoothReplyRunnable* aRunnable)
{
  return SendDiscoveryMessage(aAdapterPath, "StopDiscovery", aRunnable);
}
 
nsresult
BluetoothDBusService::StartDiscoveryInternal(const nsAString& aAdapterPath,
                                             BluetoothReplyRunnable* aRunnable)
{
  return SendDiscoveryMessage(aAdapterPath, "StartDiscovery", aRunnable);
}

nsresult
BluetoothDBusService::GetProperties(BluetoothObjectType aType,
                                    const nsAString& aPath,
                                    BluetoothReplyRunnable* aRunnable)
{
  NS_ASSERTION(NS_IsMainThread(), "Must be called from main thread!");

  MOZ_ASSERT(aType < ArrayLength(sBluetoothDBusIfaces));
  MOZ_ASSERT(aType < ArrayLength(sBluetoothDBusPropCallbacks));
  
  const char* interface = sBluetoothDBusIfaces[aType];
  DBusCallback callback = sBluetoothDBusPropCallbacks[aType];
  
  nsRefPtr<BluetoothReplyRunnable> runnable = aRunnable;

  if (!dbus_func_args_async(mConnection,
                            1000,
                            callback,
                            (void*)aRunnable,
                            NS_ConvertUTF16toUTF8(aPath).get(),
                            interface,
                            "GetProperties",
                            DBUS_TYPE_INVALID)) {
    NS_WARNING("Could not start async function!");
    return NS_ERROR_FAILURE;
  }
  runnable.forget();
  return NS_OK;
}

nsresult
BluetoothDBusService::SetProperty(BluetoothObjectType aType,
                                  const nsAString& aPath,
                                  const BluetoothNamedValue& aValue,
                                  BluetoothReplyRunnable* aRunnable)
{
  NS_ASSERTION(NS_IsMainThread(), "Must be called from main thread!");

  MOZ_ASSERT(aType < ArrayLength(sBluetoothDBusIfaces));
  const char* interface = sBluetoothDBusIfaces[aType];

  /* Compose the command */
  DBusMessage* msg = dbus_message_new_method_call("org.bluez",
                                                  NS_ConvertUTF16toUTF8(aPath).get(),
                                                  interface,
                                                  "SetProperty");

  if (!msg) {
    NS_WARNING("Could not allocate D-Bus message object!");
    return NS_ERROR_FAILURE;
  }

  const char* propName = NS_ConvertUTF16toUTF8(aValue.name()).get();
  if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &propName, DBUS_TYPE_INVALID)) {
    NS_WARNING("Couldn't append arguments to dbus message!");
    return NS_ERROR_FAILURE;
  }
  
  int type;
  int tmp_int;
  void* val;
  nsCString str;
  if (aValue.value().type() == BluetoothValue::Tuint32_t) {
    tmp_int = aValue.value().get_uint32_t();
    val = &tmp_int;
    type = DBUS_TYPE_UINT32;
  } else if (aValue.value().type() == BluetoothValue::TnsString) {
    str = NS_ConvertUTF16toUTF8(aValue.value().get_nsString());
    val = (void*)str.get();
    type = DBUS_TYPE_STRING;
  } else if (aValue.value().type() == BluetoothValue::Tbool) {
    tmp_int = aValue.value().get_bool() ? 1 : 0;
    val = &(tmp_int);
    type = DBUS_TYPE_BOOLEAN;
  } else {
    NS_WARNING("Property type not handled!");
    dbus_message_unref(msg);
    return NS_ERROR_FAILURE;
  }
  
  DBusMessageIter value_iter, iter;
  dbus_message_iter_init_append(msg, &iter);
  char var_type[2] = {(char)type, '\0'};
  if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, var_type, &value_iter) ||
      !dbus_message_iter_append_basic(&value_iter, type, val) ||
      !dbus_message_iter_close_container(&iter, &value_iter)) {
    NS_WARNING("Could not append argument to method call!");
    dbus_message_unref(msg);
    return NS_ERROR_FAILURE;
  }
  nsRefPtr<BluetoothReplyRunnable> runnable = aRunnable;

  // msg is unref'd as part of dbus_func_send_async 
  if (!dbus_func_send_async(mConnection,
                            msg,
                            1000,
                            GetVoidCallback,
                            (void*)aRunnable)) {
    NS_WARNING("Could not start async function!");
    return NS_ERROR_FAILURE;
  }
  runnable.forget();
  return NS_OK;
}

nsString
GetObjectPathFromAddress(const nsAString& aAdapterPath,
                         const nsAString& aDeviceAddress)
{
  // The object path would be like /org/bluez/2906/hci0/dev_00_23_7F_CB_B4_F1,
  // and the adapter path would be the first part of the object path, accoring
  // to the example above, it's /org/bluez/2906/hci0.
  nsString devicePath(aAdapterPath);
  devicePath.AppendLiteral("/dev_");
  devicePath.Append(aDeviceAddress);
  devicePath.ReplaceChar(':', '_');
  return devicePath;
}

bool
BluetoothDBusService::GetDevicePath(const nsAString& aAdapterPath,
                                    const nsAString& aDeviceAddress,
                                    nsAString& aDevicePath)
{
  aDevicePath = GetObjectPathFromAddress(aAdapterPath, aDeviceAddress);
  return true;
}

int
BluetoothDBusService::GetDeviceServiceChannelInternal(const nsAString& aObjectPath,
                                                      const nsAString& aPattern,
                                                      int aAttributeId)
{
  // This is a blocking call, should not be run on main thread.
  MOZ_ASSERT(!NS_IsMainThread());

  const char* deviceObjectPath = NS_ConvertUTF16toUTF8(aObjectPath).get();
  const char* pattern = NS_ConvertUTF16toUTF8(aPattern).get();

  DBusMessage *reply =
    dbus_func_args(gThreadConnection->GetConnection(),
                   deviceObjectPath,
                   DBUS_DEVICE_IFACE, "GetServiceAttributeValue",
                   DBUS_TYPE_STRING, &pattern,
                   DBUS_TYPE_UINT16, &aAttributeId,
                   DBUS_TYPE_INVALID);

  return reply ? dbus_returns_int32(reply) : -1;
}

static void
ExtractHandles(DBusMessage *aReply, nsTArray<PRUint32>& aOutHandles)
{
  uint32_t* handles = NULL;
  int len;

  DBusError err;
  dbus_error_init(&err);

  if (dbus_message_get_args(aReply, &err,
                            DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32, &handles, &len,
                            DBUS_TYPE_INVALID)) {
     if (!handles) {
       LOG("Null array in extract_handles");
     } else {
        for (int i = 0; i < len; ++i) {
        aOutHandles.AppendElement(handles[i]);
      }
    }
  } else {
    LOG_AND_FREE_DBUS_ERROR(&err);
  }
}

nsTArray<PRUint32>
BluetoothDBusService::AddReservedServicesInternal(const nsAString& aAdapterPath,
                                                  const nsTArray<PRUint32>& aServices)
{
  MOZ_ASSERT(!NS_IsMainThread());

  nsTArray<PRUint32> ret;
  const char* adapterPath = NS_ConvertUTF16toUTF8(aAdapterPath).get();

  int length = aServices.Length();
  if (length == 0) return ret;

  const uint32_t* services = aServices.Elements();
  DBusMessage* reply =
    dbus_func_args(gThreadConnection->GetConnection(),
                   adapterPath,
                   DBUS_ADAPTER_IFACE, "AddReservedServiceRecords",
                   DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32,
                   &services, length, DBUS_TYPE_INVALID);

  if (!reply) {
    LOG("Null DBus message. Couldn't extract handles.");
    return ret;
  }

  ExtractHandles(reply, ret);
  return ret;
}

bool
BluetoothDBusService::RemoveReservedServicesInternal(const nsAString& aAdapterPath,
                                                     const nsTArray<PRUint32>& aServiceHandles)
{
  MOZ_ASSERT(!NS_IsMainThread());

  int length = aServiceHandles.Length();
  if (length == 0) return false;

  const uint32_t* services = aServiceHandles.Elements();

  DBusMessage* reply =
    dbus_func_args(gThreadConnection->GetConnection(),
                   NS_ConvertUTF16toUTF8(aAdapterPath).get(),
                   DBUS_ADAPTER_IFACE, "RemoveReservedServiceRecords",
                   DBUS_TYPE_ARRAY, DBUS_TYPE_UINT32,
                   &services, length, DBUS_TYPE_INVALID);

  if (!reply) return false;

  dbus_message_unref(reply);
  return true;
}
