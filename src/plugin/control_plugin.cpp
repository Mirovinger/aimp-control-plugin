// Copyright (c) 2014, Alexey Ivanov

#include "stdafx.h"
#include "control_plugin.h"
#include "aimp/manager2.6.h"
#include "aimp/manager3.0.h"
#include "aimp/manager3.1.h"
#include "aimp/manager3.6.h"
#include "logger.h"
#include "settings.h"
#include "rpc/methods.h"
#include "rpc/compatibility/webctrl_plugin.h"
#include "rpc/frontend.h"
#include "rpc/request_handler.h"
#include "xmlrpc/frontend.h"
#include "jsonrpc/frontend.h"
#include "webctlrpc/frontend.h"
#include "http_server/request_handler.h"
#include "http_server/request_handler.h"
#include "http_server/server.h"
#include "http_server/mpfd_parser_factory.h"
#include "download_track/request_handler.h"
#include "upload_track/request_handler.h"
#include "utils/string_encoding.h"

#include <FreeImagePlus.h>
#include <winerror.h>
#include <Delayimp.h>

#include <Guiddef.h>

namespace ControlPlugin
{

AIMP2ControlPlugin* plugin2_instance = nullptr;
AIMP3ControlPlugin* plugin3_instance = nullptr;
AIMP36ControlPlugin* plugin36_instance = nullptr;
AIMPControlPlugin* plugin_instance = nullptr;

} // namespace ControlPlugin

/* Plugin DLL export function that will be called by AIMP(AIMP2 SDK). */
BOOL WINAPI AIMP_QueryAddonEx(AIMP2SDK::IAIMPAddonHeader **newAddon)
{
    using ControlPlugin::plugin2_instance;
    if (!plugin2_instance) {
        plugin2_instance = new ControlPlugin::AIMP2ControlPlugin();
    }
    plugin2_instance->AddRef();
    *newAddon = plugin2_instance;
    return TRUE;
}

/* Plugin DLL export function that will be called by AIMP(AIMP3 SDK). */
BOOL WINAPI AIMP_QueryAddon3(AIMP3SDK::IAIMPAddonPlugin** newAddon)
{
    using ControlPlugin::plugin3_instance;
    if (!plugin3_instance) {
        plugin3_instance = new ControlPlugin::AIMP3ControlPlugin();
    }
    plugin3_instance->AddRef();
    *newAddon = plugin3_instance;
    return TRUE;
}

HRESULT WINAPI AIMPPluginGetHeader(AIMP36SDK::IAIMPPlugin** Header)
{
    using ControlPlugin::plugin36_instance;
    if (!plugin36_instance) {
        plugin36_instance = new ControlPlugin::AIMP36ControlPlugin();
    }
    plugin36_instance->AddRef();
    *Header = plugin36_instance;
    return S_OK;
}

namespace {
using namespace ControlPlugin::PluginLogger;
ModuleLoggerType& logger()
    { return getLogManager().getModuleLogger<ControlPlugin::AIMPControlPlugin>(); }
}

namespace ControlPlugin
{

const std::wstring AIMPControlPlugin::kPLUGIN_SHORT_NAME        = L"Control Plugin";
const std::wstring AIMPControlPlugin::kPLUGIN_AUTHOR            = L"Alexey Ivanov";
const std::wstring AIMPControlPlugin::kPLUGIN_INFO              = L"Provides network access to AIMP player";
const std::wstring AIMPControlPlugin::kPLUGIN_SETTINGS_FILENAME = L"settings.dat";

const UINT_PTR kTickTimerEventID = 0x01020304;
const UINT     kTickTimerElapse = 100; // 100 ms.

namespace PluginLogger
{

PluginLogger::LogManager& getLogManager()
{
    return AIMPControlPlugin::getLogManager();
}

} // namespace PluginLogger

PluginLogger::LogManager& AIMPControlPlugin::getLogManager()
{
    return plugin_instance->plugin_logger_;
}

const PluginSettings::Settings& AIMPControlPlugin::settings()
{
    return plugin_instance->settings_manager_.settings();
}

boost::filesystem::wpath AIMPControlPlugin::getPluginDirectoryPath()
{
    return plugin_instance->getPluginDirectoryPath( plugin_instance->getAimpPluginsPath() );
}

AIMPControlPlugin::AIMPControlPlugin()
    :
    free_image_dll_is_available_(false),
    tick_timer_id_(0)
{
    plugin_instance = this;
}

AIMPControlPlugin::~AIMPControlPlugin()
{
    plugin_instance = nullptr;    
}

std::wstring AIMPControlPlugin::getAimpPath(int path_id) const
{
    // by default return "".
    WCHAR buffer[MAX_PATH + 1] = {0};

    if (aimp2_controller_) {
        using namespace AIMP2SDK;

        IAIMP2Extended* extended = nullptr;
        if ( aimp2_controller_->AIMP_QueryObject(IAIMP2ExtendedID, &extended) ) {
            boost::intrusive_ptr<IAIMP2Extended> aimp_extended(extended, false);
            
            const int buffer_length = aimp_extended->AIMP_GetPath(path_id, buffer, MAX_PATH);

            if (0 < buffer_length && buffer_length <= MAX_PATH) {
                return std::wstring(buffer, buffer + buffer_length);
            } else {
                // Do nothing here because of logger is not initialized at this point. Default value will be returned.
                //BOOST_LOG_SEV(logger(), error) << "Failed to get path to plugin configuration directory. AIMP_GetPath() returned " << buffer_length;
            }
        } else {
            // Do nothing here because of logger is not initialized at this point. Default value will be returned.
            //BOOST_LOG_SEV(logger(), error) << "Failed to get path to plugin configuration directory. Failed to get IAIMP2ExtendedID object.
        }
    }

    if (aimp3_core_unit_) {
        using namespace AIMP3SDK;

        IAIMPAddonsPlayerManager* manager;
        if (S_OK == aimp3_core_unit_->QueryInterface(IID_IAIMPAddonsPlayerManager, 
                                                     reinterpret_cast<void**>(&manager)
                                                     ) 
            )
        {
            boost::intrusive_ptr<IAIMPAddonsPlayerManager> player_manager(manager, false);

            if ( S_OK == player_manager->ConfigGetPath(path_id, buffer, MAX_PATH) ) {
                return buffer;
            }
        }
    }

    if (aimp36_core_) {
        AIMP36SDK::IAIMPString* path_tmp;
        if (S_OK == aimp36_core_->GetPath(path_id, &path_tmp)) {
            AIMP36SDK::IAIMPString_ptr path(path_tmp, false);
            return std::wstring(path->GetData(), path->GetLength());
        }
    }

    return L"";
}

std::wstring AIMPControlPlugin::getAimpProfilePath()
{
    const int profile_path_id = aimp2_controller_ ? AIMP2SDK::AIMP_CFG_DATA
                                                  : aimp36_core_ ? AIMP36SDK::AIMP_CORE_PATH_PROFILE
                                                                 : AIMP3SDK::AIMP_CFG_PATH_PROFILE;
    return getAimpPath(profile_path_id);
}

std::wstring AIMPControlPlugin::getAimpPluginsPath() const
{
    const int plugins_path_id = aimp2_controller_ ? AIMP2SDK::AIMP_CFG_PLUGINS
                                                  : aimp36_core_ ? AIMP36SDK::AIMP_CORE_PATH_PLUGINS
                                                                 : AIMP3SDK::AIMP_CFG_PATH_PLUGINS;
    return getAimpPath(plugins_path_id);
}

boost::filesystem::wpath AIMPControlPlugin::getPluginDirectoryPath(const boost::filesystem::wpath& base_directory) const
{   
    return base_directory / kPLUGIN_SHORT_NAME;
}

boost::filesystem::wpath AIMPControlPlugin::getSettingsFilePath(const boost::filesystem::wpath& base_directory) const
{
    return base_directory / kPLUGIN_SETTINGS_FILENAME;
}

void appendPathToPathEnvironmentVariable(boost::filesystem::wpath path)
{
    const LPCTSTR path_env = L"PATH"; 
    const DWORD buffer_size = GetEnvironmentVariable(path_env, nullptr, 0);
    if (buffer_size > 0) {
        std::wstring original_path(buffer_size - 1, '\0');
		GetEnvironmentVariable(path_env, const_cast<LPWSTR>(original_path.c_str()), buffer_size);
		std::wstring new_path = path.normalize().native() + L";" + original_path;
		SetEnvironmentVariable(path_env, new_path.c_str());
    }
}

// Directory will be created if it doesn't exist.
bool isDirectoryWriteEnabled(const boost::filesystem::wpath& directory)
{
    // ensure directory exists or can be created
    try {
        /*  Logic is simple: we try to create plugin_work_directory instead of check if directory existing.
            fs::create_directory() will return false without exception if directory already exists or it returns true if directory is created successfully.
            We avoid code of existing check because of we need to create plugin work directory any way. */
        fs::create_directory(directory);
    } catch (fs::filesystem_error&) {
        // "directory can not be created."
        return false;
    }

    boost::filesystem::wpath test_file_path = directory / L"testdiraccess";
    // ensure we can create file
    std::wofstream temp_file( test_file_path.c_str() );
    if ( !temp_file.is_open() ) {
        // "Log directory is read only."
        return false;
    }
    temp_file.close();

    // ensure we can remove files
    try {
        remove(test_file_path);
    } catch (fs::filesystem_error&) {
        // "Test file can not be removed. Directory should provide full access for correct work."
        return false;
    }

    // ok, we have rights for directory modification.
    return true;
}

void AIMPControlPlugin::ensureWorkDirectoryExists()
{
    namespace fs = boost::filesystem;

    // check Plugins directory first, use it if it's writable.
    const fs::wpath plugins_subdirectory = getPluginDirectoryPath( getAimpPluginsPath() ),
                    profile_subdirectory = getPluginDirectoryPath( getAimpProfilePath() );
    if ( isDirectoryWriteEnabled(plugins_subdirectory) ) {
        plugin_work_directory_ = plugins_subdirectory;
    } else if ( isDirectoryWriteEnabled(profile_subdirectory) ) {
        plugin_work_directory_ = profile_subdirectory;
    } else {
        plugin_work_directory_ = plugins_subdirectory; // set work directory in any case.

        using namespace StringEncoding;
        // work directory is not accessible for writing or does not exist.
        // TODO: send log to aimp internal logger.
        BOOST_LOG_SEV(logger(), error) << "Neither \""
                                       << utf16_to_system_ansi_encoding_safe( plugins_subdirectory.native() )
                                       << "\", nor \"" 
                                       << utf16_to_system_ansi_encoding_safe( profile_subdirectory.native() )
                                       << "\" are accessible for writing. Use plugins subdirectory as work directory."; 
    }
}

void AIMPControlPlugin::loadSettings()
{
    // Note: logger is not available at this point.
    namespace fs = boost::filesystem;

    const fs::wpath settings_in_plugins_filepath = getSettingsFilePath( getPluginDirectoryPath( getAimpPluginsPath() ) );
    plugin_settings_filepath_ = settings_in_plugins_filepath;
    if ( fs::exists(settings_in_plugins_filepath) ) {
        // load settings from file.
        try {
            settings_manager_.load(settings_in_plugins_filepath);
        } catch (std::exception&) {
            // settings file reading failed. Default settings will be used.
        }
    } else {
        // For seamless transition from old version(1.0.7.825 and previous)
        // we try to load settings from aimp profile directory.
        const fs::wpath settings_in_profile_filepath = getSettingsFilePath( getPluginDirectoryPath( getAimpProfilePath() ) );
        if ( fs::exists(settings_in_profile_filepath) ) {
            try {
                settings_manager_.load(settings_in_profile_filepath);
                // after loading settings from old plugin work directory we will save settings to current work directory.
            } catch (std::exception&) {
                // old settings file reading failed. Default settings will be used.
            }
        }

        try {
            // save the default settings file.
            settings_manager_.save(settings_in_plugins_filepath);
        } catch (std::exception&) {
            // settings file saving failed.
            // TODO: send to internal AIMP log.
        }
    }
}

void AIMPControlPlugin::initializeLogger()
{
    if (settings().logger.severity_level < PluginLogger::severity_levels_count) {
        plugin_logger_.setSeverity(settings().logger.severity_level);
        // get absolute path to log directory.
        boost::filesystem::wpath log_directory = settings().logger.directory;
        if ( !log_directory.is_complete() ) {
            log_directory = plugin_work_directory_ / log_directory;
        }

        try {
            plugin_logger_.startLog(log_directory,
                                    settings().logger.modules_to_log
                                   );
        } catch (FileLogError& e) {
            // file log is impossible.
            assert(!"File log was not initializated.");
            // Send msg to other log backends.
            BOOST_LOG_SEV(logger(), error)  << "File log was not initializated, "
                                            << "log directory "
                                            << StringEncoding::utf16_to_system_ansi_encoding_safe( log_directory.native() )
                                            << ". Reason: "
                                            << e.what();
        }
    }
}

void AIMPControlPlugin::Initialize(AIMP2SDK::IAIMP2Controller* AController)
{
    aimp2_controller_.reset(AController);
    initialize();
}

HRESULT AIMPControlPlugin::Initialize(AIMP3SDK::IAIMPCoreUnit* ACoreUnit)
{
    aimp3_core_unit_.reset(ACoreUnit);
    return initialize();
}

HRESULT AIMPControlPlugin::Initialize(AIMP36SDK::IAIMPCore* Core)
{
    aimp36_core_.reset(Core);
    return initialize();
}

PWCHAR AIMPControlPlugin::InfoGet(int index)
{
    using namespace AIMP36SDK;

    switch (index) {
    case AIMP_PLUGIN_INFO_NAME:
        return const_cast<const PWCHAR>( AIMPControlPlugin::kPLUGIN_SHORT_NAME.c_str() ); // const cast is safe here since AIMP does not try to modify these data.
    case AIMP_PLUGIN_INFO_AUTHOR:
        return const_cast<PWCHAR>( AIMPControlPlugin::kPLUGIN_AUTHOR.c_str() ); // const cast is safe here since AIMP does not try to modify these data.
    case AIMP_PLUGIN_INFO_SHORT_DESCRIPTION:
        return const_cast<PWCHAR>( AIMPControlPlugin::kPLUGIN_INFO.c_str() ); // const cast is safe here since AIMP does not try to modify these data.
    case AIMP_PLUGIN_INFO_FULL_DESCRIPTION:
        // no full description.
        break;
    }
    return nullptr;
}

void AIMPControlPlugin::SystemNotification(int notifyID, IUnknown* data) {
    // Do nothing here because plugin depends on base services only. They can't be disabled in runtime.
    // Don't forget to catch all exceptions here when add real code here.
    using namespace AIMP36SDK;

    switch (notifyID) {
    case AIMP_SYSTEM_NOTIFICATION_SERVICE_ADDED:
        BOOST_LOG_SEV(logger(), debug) << "SystemNotification(): NotifyID = AIMP_SYSTEM_NOTIFICATION_SERVICE_ADDED, Data = " << (void*)data;
        break;
    case AIMP_SYSTEM_NOTIFICATION_SERVICE_REMOVED:
        BOOST_LOG_SEV(logger(), debug) << "SystemNotification(): NotifyID = AIMP_SYSTEM_NOTIFICATION_SERVICE_REMOVED, Data = " << (void*)data;
        break;
    case AIMP_SYSTEM_NOTIFICATION_EXTENSION_REMOVED:
        BOOST_LOG_SEV(logger(), debug) << "SystemNotification(): NotifyID = AIMP_SYSTEM_NOTIFICATION_EXTENSION_REMOVED, Data = " << (void*)data;
        break;
    default:
        BOOST_LOG_SEV(logger(), error) << "SystemNotification(): unexpected NotifyID = " << notifyID << ", Data = " << (void*)data;
        break;
    }
}

int getAIMPVersion(AIMP3SDK::IAIMPCoreUnit* aimp3_core_unit)
{
    using namespace AIMP3SDK;
    TAIMPVersionInfo version_info = {0};
    HRESULT r = aimp3_core_unit->GetVersion(&version_info);

    if (S_OK != r) {
        BOOST_LOG_SEV(logger(), error) << "IAIMPCoreUnit::GetVersion returned " << r;
        throw std::runtime_error("Unable to extract AIMP version. "__FUNCTION__);
    }
    
    return version_info.ID;
}

boost::shared_ptr<AIMPPlayer::AIMPManager> AIMPControlPlugin::CreateAIMPManager()
{
    boost::shared_ptr<AIMPPlayer::AIMPManager> result;
    if (aimp2_controller_) {
        result.reset( new AIMPPlayer::AIMPManager26(aimp2_controller_, *server_io_service_) );
    } else if (aimp3_core_unit_) {
        const int version = getAIMPVersion(aimp3_core_unit_.get());
        if (version >= 3100) {
            result.reset( new AIMPPlayer::AIMPManager31(aimp3_core_unit_, *server_io_service_) );
        } else {
            result.reset( new AIMPPlayer::AIMPManager30(aimp3_core_unit_, *server_io_service_) );
        }
    } else if (aimp36_core_) {
        result.reset( new AIMPPlayer::AIMPManager36(aimp36_core_, *server_io_service_) );
    } else {
        assert(!"both AIMP2 and AIMP3 plugin addon objects do not exist.");
        throw std::runtime_error("both AIMP2 and AIMP3 plugin addon objects do not exist. "__FUNCTION__);
    }
    return result;
}

HRESULT AIMPControlPlugin::initialize()
{
    HRESULT result = S_OK;

    ensureWorkDirectoryExists();
    loadSettings(); // If file does not exist tries to save default settings.
    initializeLogger();

    BOOST_LOG_SEV(logger(), info) << "Plugin initialization is started";

    // freeimage DLL loading
    appendPathToPathEnvironmentVariable( getPluginDirectoryPath( getAimpPluginsPath() ) ); // make possible to load FreeImage dll from plugin directory.
    checkFreeImageDLLAvailability();

    // create plugin core
    try {
        server_io_service_ = boost::make_shared<boost::asio::io_service>();

        // create AIMP manager.
        aimp_manager_ = CreateAIMPManager();

        BOOST_LOG_SEV(logger(), info) << "AIMP version: " << aimp_manager_->getAIMPVersion();
        BOOST_LOG_SEV(logger(), info) << "Plugin version: " << StringEncoding::utf16_to_utf8( Utilities::getPluginVersion() );

        // create RPC request handler.
        rpc_request_handler_.reset( new Rpc::RequestHandler() );
        createRpcFrontends();
        createRpcMethods();

        download_track_request_handler_.reset( new DownloadTrack::RequestHandler(*aimp_manager_) );

        {
            if (settings().misc.enable_track_upload) {
                // Use custom tmp dir path getter to avoid issue with junction point as tmp dir.
                const fs::wpath temp_dir_to_store_tracks_being_added = Utilities::temp_directory_path() / kPLUGIN_SHORT_NAME;
            
                fs::create_directories(temp_dir_to_store_tracks_being_added);

                Http::MPFD::ParserFactory::instance(Http::MPFD::ParserFactory::ParserFactoryPtr(new Http::MPFD::ParserFactoryImpl(temp_dir_to_store_tracks_being_added))
                                                    );
            }

            upload_track_request_handler_.reset( new UploadTrack::RequestHandler(*aimp_manager_,
                                                                                 settings().misc.enable_track_upload
                                                                                 )
                                                );
        }

        using namespace StringEncoding;
        // create HTTP request handler.
        http_request_handler_.reset( new Http::RequestHandler( utf16_to_system_ansi_encoding( getWebServerDocumentRoot().native() ),
                                                               *rpc_request_handler_,
                                                               *download_track_request_handler_,
                                                               *upload_track_request_handler_
                                                              )
                                    );
        // create XMLRPC server.
        server_.reset(new Http::Server( *server_io_service_,
                                        *http_request_handler_
                                       )
                      );

        startTickTimer();
    } catch (boost::thread_resource_error& e) {
        BOOST_LOG_SEV(logger(), critical) << "Plugin initialization failed. Reason: create main server thread failed. Reason: " << e.what();
        result = E_FAIL;
    } catch (std::runtime_error& e) {
        BOOST_LOG_SEV(logger(), critical) << "Plugin initialization failed. Reason: " << e.what();
        result = E_FAIL;
    } catch (...) {
        BOOST_LOG_SEV(logger(), critical) << "Plugin initialization failed. Reason is unknown";
        result = E_FAIL;
    }

    BOOST_LOG_SEV(logger(), info) << "Plugin initialization is finished";

    return result;
}

HRESULT AIMPControlPlugin::Finalize()
{
    BOOST_LOG_SEV(logger(), info) << "Plugin finalization is started";

    stopTickTimer();

    server_io_service_->stop();
    
    if (server_) {
        // stop the server.
        BOOST_LOG_SEV(logger(), info) << "Stopping server.";

        // destroy the server.
        server_.reset();
    }

    http_request_handler_.reset();

    download_track_request_handler_.reset();

    upload_track_request_handler_.reset();

    rpc_request_handler_.reset();

    aimp_manager_.reset();

    aimp2_controller_.reset();
    aimp3_core_unit_.reset();
    aimp36_core_.reset();


    server_io_service_.reset();

    BOOST_LOG_SEV(logger(), info) << "Plugin finalization is finished";

    plugin_logger_.stopLog();

    return S_OK;
}

HRESULT AIMPControlPlugin::ShowSettingsDialog(HWND AParentWindow)
{
    STARTUPINFO si = { 0 };
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = { 0 };

    const std::wstring settingsManagerWorkDir = L"Control Plugin\\SettingsManager";
    const std::wstring settingsManagerExecutable = L"Control Plugin\\SettingsManager\\SettingsManager.exe";
    BOOL success = CreateProcess(   settingsManagerExecutable.c_str(), 
                                    NULL,           // No command line
                                    NULL,           // Process handle not inheritable
                                    NULL,           // Thread handle not inheritable
                                    FALSE,          // Set handle inheritance to FALSE
                                    0,              // No creation flags
                                    NULL,           // Use parent's environment block
                                    settingsManagerWorkDir.c_str(),
                                    &si,            // Pointer to STARTUPINFO structure
                                    &pi             // Pointer to PROCESS_INFORMATION structure
                                );
    if (success) {
        BOOST_LOG_SEV(logger(), debug) << "SettingsManager has been launched";
    } else {
        BOOST_LOG_SEV(logger(), error) << "SettingsManager launch failed. Error " << GetLastError();

        char currentDir[MAX_PATH];
        GetCurrentDirectoryA(MAX_PATH, currentDir);
        BOOST_LOG_SEV(logger(), debug) << "currentDir: " << currentDir;

        std::wostringstream message_body;
        message_body << L"AIMP Control plugin settings can be found in configuration file " << plugin_settings_filepath_;
        MessageBox( AParentWindow,
                    message_body.str().c_str(),
                    L"Information about AIMP Control Plugin",
                    MB_ICONINFORMATION);
    }
    return S_OK;
}

void AIMPControlPlugin::createRpcFrontends()
{
#define REGISTER_RPC_FRONTEND(name) rpc_request_handler_->addFrontend( std::auto_ptr<Rpc::Frontend>( \
                                                                                                    new name::Frontend() \
                                                                                                    )\
                                                                      )
    REGISTER_RPC_FRONTEND(XmlRpc);
    REGISTER_RPC_FRONTEND(JsonRpc);
    REGISTER_RPC_FRONTEND(WebCtlRpc);
#undef REGISTER_RPC_FRONTEND
}

void AIMPControlPlugin::createRpcMethods()
{
    using namespace AimpRpcMethods;

#define REGISTER_AIMP_RPC_METHOD(method_type) \
            rpc_request_handler_->addMethod( \
                            std::auto_ptr<Rpc::Method>( \
                                                        new method_type(*aimp_manager_, \
                                                                        *rpc_request_handler_\
                                                                        ) \
                                                       ) \
                                            )
    // control panel
    REGISTER_AIMP_RPC_METHOD(Play);
    REGISTER_AIMP_RPC_METHOD(Pause);
    REGISTER_AIMP_RPC_METHOD(Stop);
    REGISTER_AIMP_RPC_METHOD(PlayPrevious);
    REGISTER_AIMP_RPC_METHOD(PlayNext);
    REGISTER_AIMP_RPC_METHOD(ShufflePlaybackMode);
    REGISTER_AIMP_RPC_METHOD(RepeatPlaybackMode);
    REGISTER_AIMP_RPC_METHOD(VolumeLevel);
    REGISTER_AIMP_RPC_METHOD(Mute);
    REGISTER_AIMP_RPC_METHOD(RadioCaptureMode);
    REGISTER_AIMP_RPC_METHOD(Status);
    REGISTER_AIMP_RPC_METHOD(GetPlayerControlPanelState);
    // playlists
    REGISTER_AIMP_RPC_METHOD(GetPlaylists);
	REGISTER_AIMP_RPC_METHOD(CreatePlaylist);
    // tracks
    REGISTER_AIMP_RPC_METHOD(EnqueueTrack);
    REGISTER_AIMP_RPC_METHOD(RemoveTrackFromPlayQueue);
    REGISTER_AIMP_RPC_METHOD(QueueTrackMove);
    
    {
    // pass io service
    rpc_request_handler_->addMethod( std::auto_ptr<Rpc::Method>(
                                            new RemoveTrack(*aimp_manager_,
                                                            *rpc_request_handler_,
                                                            *server_io_service_
                                                            )
                                                                )
                                    );
    }

    { // register this way since GetEntryPositionInDataTable and GetQueuedEntries depend on GetPlaylistEntries.
    std::auto_ptr<GetPlaylistEntries> method_getplaylistentries(new GetPlaylistEntries(*aimp_manager_,
                                                                                       *rpc_request_handler_
                                                                                       )
                                                                );

    std::auto_ptr<Rpc::Method> method_getentrypositionindatatable(new GetEntryPositionInDataTable(*aimp_manager_,
                                                                                                  *rpc_request_handler_,
                                                                                                  *method_getplaylistentries
                                                                                                  )
                                                                  );
    std::auto_ptr<Rpc::Method> method_getqueuedentries(new GetQueuedEntries(*aimp_manager_,
                                                                            *rpc_request_handler_,
                                                                            *method_getplaylistentries
                                                                            )
                                                        );
    { // auto_ptr can not be implicitly casted to ptr to object of base class.
    std::auto_ptr<Rpc::Method> method( method_getplaylistentries.release() );
    rpc_request_handler_->addMethod(method);
    }
    rpc_request_handler_->addMethod(method_getentrypositionindatatable);
    rpc_request_handler_->addMethod(method_getqueuedentries);
    }

    REGISTER_AIMP_RPC_METHOD(GetPlaylistEntriesCount);
    REGISTER_AIMP_RPC_METHOD(GetFormattedEntryTitle);
    REGISTER_AIMP_RPC_METHOD(GetPlaylistEntryInfo);

    // track's album cover.
    try {
        const bool aimp_support_reading_cover_directly_from_external_file = ControlPlugin::plugin2_instance == nullptr;
        if (!aimp_support_reading_cover_directly_from_external_file && !free_image_dll_is_available_) {
            throw std::runtime_error("FreeImage DLL is not available and AIMP2 does not support direct access to album covers.");
        }

        // add document root and path to directory for storing album covers in GetCover method.
        rpc_request_handler_->addMethod( std::auto_ptr<Rpc::Method>(
                                                new GetCover(*aimp_manager_,
                                                             *rpc_request_handler_,
                                                             getWebServerDocumentRoot(),
                                                             L"album_covers_cache", // directory in document root to store temp image files.
                                                             free_image_dll_is_available_
                                                             )
                                                                    )
                                        );
    } catch(std::exception& e) {
        BOOST_LOG_SEV(logger(), info) << "Album cover processing was disabled. Reason: " << e.what();
    } catch (...) {
        BOOST_LOG_SEV(logger(), info) << "Album cover processing was disabled. Reason unknown.";
    }

    // Comet technique, "subscribe" method.
    REGISTER_AIMP_RPC_METHOD(SubscribeOnAIMPStateUpdateEvent);
    // add file name for rating store file to SetTrackRating() method.
    rpc_request_handler_->addMethod( std::auto_ptr<Rpc::Method>(
                                                    new SetTrackRating( *aimp_manager_,
                                                                        *rpc_request_handler_,
                                                                        (plugin_work_directory_ / L"rating_store.txt").native()
                                                                       )
                                                                )
                                    );
    REGISTER_AIMP_RPC_METHOD(Version);
    REGISTER_AIMP_RPC_METHOD(PluginCapabilities);
    REGISTER_AIMP_RPC_METHOD(AddURLToPlaylist);

    {
    // pass io service
    rpc_request_handler_->addMethod( std::auto_ptr<Rpc::Method>(
                                                  new Scheduler(*aimp_manager_,
                                                                *rpc_request_handler_,
                                                                *server_io_service_
                                                                )
                                                                )
                                    );
    }

    // Emulator of Aimp WebCtl plugin.
    REGISTER_AIMP_RPC_METHOD(EmulationOfWebCtlPlugin);
#undef REGISTER_AIMP_RPC_METHOD
}

LONG WINAPI DelayLoadDllExceptionFilter(PEXCEPTION_POINTERS pep)
{
    // Assume we recognize this exception
    LONG lDisposition = EXCEPTION_EXECUTE_HANDLER;

    // If this is a Delay-load problem, ExceptionInformation[0] points
    // to a DelayLoadInfo structure that has detailed error info
    PDelayLoadInfo pdli = PDelayLoadInfo(pep->ExceptionRecord->ExceptionInformation[0]);

#ifndef FACILITY_VISUALCPP
#define FACILITY_VISUALCPP (109)
#endif

    switch (pep->ExceptionRecord->ExceptionCode)
    {
    case VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND):
        // The DLL module was not found at runtime
        BOOST_LOG_SEV(logger(), warning) << "Dll " << pdli->szDll << " not found.";
        break;

    case VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND):
        // The DLL module was found, but it doesn't contain the function
        if (pdli->dlp.fImportByName) {
            BOOST_LOG_SEV(logger(), warning) << "Function " << pdli->dlp.szProcName << " was not found in " << pdli->szDll;
        } else {
            BOOST_LOG_SEV(logger(), warning) << "Function ordinal " << pdli->dlp.dwOrdinal << " was not found in " << pdli->szDll;
        }
        break;

    default:
        // We don't recognize this exception
        lDisposition = EXCEPTION_CONTINUE_SEARCH;
        break;
    }

    return lDisposition;
}

void freeImagePlusDllTest()
{
    fipWinImage img; // check fipWinImage::fipWinImage() availability.
    img.copyFromBitmap(0); // check fipWinImage::copyFromBitmap() availability.
    FreeImageIO io;
    img.saveToHandle(FIF_PNG, &io, nullptr); // check fipWinImage::saveToHandle() availability.
}

void AIMPControlPlugin::checkFreeImageDLLAvailability()
{
    // Wrap all calls to delay-load DLL functions inside SEH
    __try {
        freeImagePlusDllTest();
        free_image_dll_is_available_ = true; // if we are here - FreeImagePlus dll was loaded fine, allow using.
    } __except ( DelayLoadDllExceptionFilter( GetExceptionInformation() ) ) {
        // DLL was not loaded, using of FreeImagePlus functions is forbidden.
    }
}

boost::filesystem::wpath AIMPControlPlugin::getWebServerDocumentRoot() const // throws std::runtime_error
{
    // get document root from settings.
    namespace fs = boost::filesystem;
    fs::wpath document_root_path = settings().http_server.document_root;
    if ( !document_root_path.is_complete() ) {
        document_root_path = plugin_work_directory_ / document_root_path;
    }

    if ( !( fs::exists(document_root_path) && fs::is_directory(document_root_path) ) ) {
        throw std::runtime_error(Utilities::MakeString() << "Web-server document root directory does not exist: \"" 
                                                         << StringEncoding::utf16_to_system_ansi_encoding( document_root_path.native() ) << "\""
                                 );
    }

    return document_root_path;
}

void AIMPControlPlugin::startTickTimer()
{
    tick_timer_id_ = ::SetTimer(NULL, kTickTimerEventID, kTickTimerElapse, &AIMPControlPlugin::onTickTimerProc);
    if (tick_timer_id_ == 0) {
        BOOST_LOG_SEV(logger(), critical) << "Plugin's service interrupted: SetTimer failed with error: " << GetLastError();
    }
}

void AIMPControlPlugin::stopTickTimer()
{
    if (tick_timer_id_ != 0) {
        if (::KillTimer(NULL, tick_timer_id_) == 0) {
            BOOST_LOG_SEV(logger(), warning) << "KillTimer failed with error: " << GetLastError();
        }
    }
}

void CALLBACK AIMPControlPlugin::onTickTimerProc(HWND /*hwnd*/,
                                                 UINT /*uMsg*/,
                                                 UINT_PTR /*idEvent*/,
                                                 DWORD /*dwTime*/)
{
    plugin_instance->onTick();
}

void AIMPControlPlugin::onTick()
{
    try {
        server_io_service_->poll();
        { // for tests
            using namespace AIMPPlayer;
            if (aimp_manager_) {
                aimp_manager_->onTick();
            }
        }
    } catch (std::exception& e) {
        // Just send error in log and stop processing.
        BOOST_LOG_SEV(logger(), critical) << "Unhandled exception inside ControlPlugin::onTick(): " << e.what();
        server_io_service_->stop();
        stopTickTimer();
        BOOST_LOG_SEV(logger(), info) << "Service was stopped.";
    }
}

} // namespace ControlPlugin
