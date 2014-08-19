## set the QTC_SOURCE environment variable to override the setting here
QTCREATOR_SOURCES = $$(QTC_SOURCE)

## set the QTC_BUILD environment variable to override the setting here
#IDE_BUILD_TREE = $$(QTC_BUILD)
IDE_BUILD_TREE =

include(cmakeprojectmanager_dependencies.pri)
include($$QTCREATOR_SOURCES/src/qtcreatorplugin.pri)

HEADERS = cmakebuildinfo.h \
    cmakeproject.h \
    cmakeprojectplugin.h \
    cmakeprojectmanager.h \
    cmakeprojectconstants.h \
    cmakeprojectnodes.h \
    makestep.h \
    cmakerunconfiguration.h \
    cmakeopenprojectwizard.h \
    cmakebuildconfiguration.h \
    cmakeeditorfactory.h \
    cmakeeditor.h \
    cmakehighlighter.h \
    cmakelocatorfilter.h \
    cmakefilecompletionassist.h \
    cmaketool.h \
    cmakeparser.h \
    generatorinfo.h \
    cmakesettingspage.h

SOURCES = cmakeproject.cpp \
    cmakeprojectplugin.cpp \
    cmakeprojectmanager.cpp \
    cmakeprojectnodes.cpp \
    makestep.cpp \
    cmakerunconfiguration.cpp \
    cmakeopenprojectwizard.cpp \
    cmakebuildconfiguration.cpp \
    cmakeeditorfactory.cpp \
    cmakeeditor.cpp \
    cmakehighlighter.cpp \
    cmakelocatorfilter.cpp \
    cmakefilecompletionassist.cpp \
    cmaketool.cpp \
    cmakeparser.cpp \
    generatorinfo.cpp \
    cmakesettingspage.cpp


RESOURCES += cmakeproject.qrc

OTHER_FILES += README.txt
