add_library(chtholly_driver_support
  Tooling/CFFIToolchain.cpp
  Tooling/DeploymentManifest.cpp
  Tooling/ComponentDeployment.cpp
  Tooling/CliOutput.cpp
  Tooling/CompilerInvocation.cpp
  Tooling/ManifestDiscovery.cpp
  Tooling/ManifestToml.cpp
  Tooling/NativeLinker.cpp
  Tooling/ProcessRunner.cpp
  Tooling/ProjectScaffold.cpp
  Tooling/ResourceLocator.cpp
  Tooling/TargetConfig.cpp)

chtholly_configure_target(chtholly_driver_support)
target_include_directories(chtholly_driver_support PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_driver_support
  PUBLIC chtholly_basic chtholly_support chtholly_tooling_rules)
if(WIN32)
  target_link_libraries(chtholly_driver_support PRIVATE advapi32)
endif()

add_library(chtholly_package_tooling
  Package/ArtifactStore.cpp
  Package/GitRepositoryCache.cpp
  Package/PackageArtifact.cpp
  Package/PackageArtifactArchive.cpp
  Package/PackageArtifactClosure.cpp
  Package/ProjectManifest.cpp
  Tooling/SemVer.cpp)

chtholly_configure_target(chtholly_package_tooling)
target_include_directories(chtholly_package_tooling PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_package_tooling
  PUBLIC chtholly_driver_support chtholly_basic chtholly_support
         chtholly_semir
  PRIVATE chtholly_vendor_miniz)

add_library(chtholly_registry_tooling
  Registry/RegistryArtifact.cpp
  Registry/RegistryCrypto.cpp
  Registry/RegistrySigner.cpp
  Registry/RegistryTransparency.cpp
  Registry/RegistryTransparencyClient.cpp
  Registry/RegistryTrust.cpp
  Registry/RegistryWitness.cpp)

chtholly_configure_target(chtholly_registry_tooling)
target_include_directories(chtholly_registry_tooling PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_registry_tooling
  PUBLIC
    chtholly_driver_support
    chtholly_package_tooling)
target_link_libraries(chtholly_registry_tooling
  PRIVATE
    unofficial-sodium::sodium
    unofficial::sqlite3::sqlite3
    CURL::libcurl)

add_library(chtholly_toolchain_tooling Toolchain/ToolchainManager.cpp
  Toolchain/ToolchainSpace.cpp
  Registry/RegistryCrypto.cpp Tooling/SemVer.cpp)
chtholly_configure_target(chtholly_toolchain_tooling)
target_include_directories(chtholly_toolchain_tooling PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_toolchain_tooling
  PUBLIC chtholly_driver_support
  PRIVATE chtholly_vendor_miniz unofficial-sodium::sodium)
add_library(chtholly_driver_compiler
  Compiler/Doctor.cpp
  Compiler/CompilerArtifactLoadExecutor.cpp
  Compiler/CompilerArtifactLoadMetrics.cpp
  Compiler/CompilerArtifactStoreAcquire.cpp
  Compiler/CompilerArtifactStore.cpp
  Compiler/CompilerArtifactStoreGC.cpp
  Compiler/CompilerArtifactStorePaths.cpp
  Compiler/CompilerArtifactStorePublish.cpp
  Compiler/CompilerArtifactStoreRead.cpp
  Compiler/CompilerArtifactStoreReference.cpp
  Compiler/CompilerArtifactStoreWrite.cpp
  Compiler/CompilerBuildControlSnapshot.cpp
  Compiler/CompilerBuildPlan.cpp
  Compiler/CompilerPipeline.cpp
  Compiler/CompilerPipelineSourceInspection.cpp
  Compiler/CompilerPipelineOutput.cpp
  Compiler/CompilerLinkClosure.cpp
  Compiler/CompilerArtifactPublication.cpp
  Compiler/CompilerPipelineExecution.cpp
  Compiler/CompilerPackageQueryExecution.cpp
  Compiler/CompilerDaemon.cpp
  Compiler/CompilerInputFileSystem.cpp
  Compiler/CompilerLanguageSupport.cpp
  Compiler/CompilerPipelinePlanning.cpp
  Compiler/CompilerPipelineDiagnostics.cpp
  Compiler/CompilerStandardLibrary.cpp
  Compiler/CompilerSourceSnapshot.cpp)
chtholly_configure_target(chtholly_driver_compiler)
target_include_directories(chtholly_driver_compiler PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_driver_compiler
  PUBLIC chtholly_driver_support chtholly_pipeline chtholly_package_tooling
  PRIVATE chtholly_semir)

add_library(chtholly_registry_server
  Registry/RegistryBackup.cpp
  Registry/RegistryDatabaseInternal.cpp
  Registry/RegistryServerSupportInternal.cpp
  Registry/RegistryServerConfig.cpp
  Registry/RegistryServer.cpp)
chtholly_configure_target(chtholly_registry_server)
target_include_directories(chtholly_registry_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_registry_server
  PUBLIC chtholly_registry_tooling
  PRIVATE chtholly_vendor_miniz unofficial-sodium::sodium
          unofficial::sqlite3::sqlite3)

add_library(chtholly_cffi_tooling CFFI/CFFIRegeneration.cpp CFFI/CFFITool.cpp)
chtholly_configure_target(chtholly_cffi_tooling)
target_include_directories(chtholly_cffi_tooling PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(chtholly_cffi_tooling
  PUBLIC chtholly_driver_support chtholly_semir
  PRIVATE chtholly_support ${CMAKE_DL_LIBS})
chtholly_link_libclang(chtholly_cffi_tooling)
