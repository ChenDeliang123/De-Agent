# ---------------------------------------------------------------------------
# CPack 打包配置（deb / rpm）
#
# 由顶层 CMakeLists 在「本项目是顶层工程」时 include。
# 只打包 deagent 组件（见顶层 install() 规则），不会带入 a2a-protocol 子项目的
# 安装物（其 install() 无 COMPONENT，落在 Unspecified，被 CPACK_COMPONENTS_ALL 排除）。
#
# 用法：
#   cmake --build build-release -j
#   ( cd build-release && cpack -G DEB )        # 或 -G RPM（需 rpmbuild）
#   # 也可用 scripts/package-deb-rpm.sh 一键出包
# ---------------------------------------------------------------------------

# 版本：复用 project(VERSION) 的单一来源。
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})

set(CPACK_PACKAGE_NAME "de-agent")
set(CPACK_PACKAGE_VENDOR "德先生")
set(CPACK_PACKAGE_CONTACT "de agent maintainers <chen_dl@gitee.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "de agent - C++ coding agent runtime with a zero-dependency terminal client")
set(CPACK_PACKAGE_DESCRIPTION
    "de agent 是一个通用型 coding agent：C++ Agent Runtime + 本地 HTTP/SSE 后端 +\n"
    "零 npm 依赖的终端客户端。接入 DeepSeek，支持 MCP 工具、权限模式与会话持久化。")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://gitee.com/chen_dl/ai-agent-cpp")

# 安装到系统前缀（deb/rpm 不应装到 /usr/local）。
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

# 只打 deagent 组件；忽略 Unspecified（a2a 子项目产物）。
set(CPACK_COMPONENTS_ALL deagent)
set(CPACK_DEB_COMPONENT_INSTALL ON)
set(CPACK_RPM_COMPONENT_INSTALL ON)

# ---------------------------------------------------------------------------
# DEB
# ---------------------------------------------------------------------------
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")
set(CPACK_DEBIAN_PACKAGE_NAME "de-agent")
set(CPACK_DEBIAN_DEAGENT_PACKAGE_NAME "de-agent")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "de agent maintainers <chen_dl@gitee.com>")
# 自动分析二进制依赖（libcurl4 / libc6 / libstdc++6 ...），与下面的显式依赖合并。
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# 运行时依赖：后端需 libcurl（SHLIBDEPS 会自动加）；终端客户端需 node >= 18 与 curl。
set(CPACK_DEBIAN_DEAGENT_PACKAGE_DEPENDS "nodejs (>= 18), curl")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "python3")

# ---------------------------------------------------------------------------
# RPM（需目标机器安装 rpmbuild；本仓库脚本会在缺失时自动跳过）
# ---------------------------------------------------------------------------
set(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
set(CPACK_RPM_PACKAGE_NAME "de-agent")
set(CPACK_RPM_DEAGENT_PACKAGE_NAME "de-agent")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
# TODO: 选定 LICENSE 后同步这里。
set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
set(CPACK_RPM_PACKAGE_REQUIRES "nodejs >= 18, curl")
set(CPACK_RPM_PACKAGE_AUTOREQ OFF)
set(CPACK_RPM_PACKAGE_SUMMARY
    "de agent - C++ coding agent runtime with a zero-dependency terminal client")

include(CPack)
