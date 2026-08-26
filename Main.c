#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/GraphicsOutput.h>
#include <Protocol/SimplePointer.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/SimpleFileSystem.h>
#include <Protocol/BlockIo.h>
#include <Library/PrintLib.h>
#include <Guid/FileInfo.h>

#if defined(_MSC_VER)
  #include <intrin.h>
  #define io_outb(port, val) __outbyte((unsigned short)(port), (unsigned char)(val))
  #define io_inb(port)       __inbyte((unsigned short)(port))
#else
  STATIC inline VOID io_outb(UINT16 port, UINT8 val) {
      __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
  }
  STATIC inline UINT8 io_inb(UINT16 port) {
      UINT8 ret;
      __asm__ __volatile__("inb %1, %0" : "=a"(ret) : "Nd"(port));
      return ret;
  }
#endif

#ifndef EFI_ABSPTR_TOUCH_OR_FIRST_BUTTON
#define EFI_ABSPTR_TOUCH_OR_FIRST_BUTTON 0x00000001
#endif
#ifndef EFI_ABSP_TouchActive
#define EFI_ABSP_TouchActive 0x00000001
#endif

// --- Color & Geometric Constants ---
#define RGB(r,g,b)          ((UINT32)(((r)<<16)|((g)<<8)|(b)))
#define COLOR_DESKTOP       RGB(30, 41, 59)
#define COLOR_TOPBAR_BG     RGB(15, 23, 42)
#define COLOR_WIN_BG        RGB(51, 65, 85)
#define COLOR_WIN_BORDER    RGB(100, 116, 139)
#define COLOR_CANVAS        RGB(15, 15, 15)
#define COLOR_TITLE_ACTIVE  RGB(37, 99, 235)
#define COLOR_BTN_CLOSE     RGB(225, 29, 72)
#define COLOR_TEXT          RGB(241, 245, 249)
#define COLOR_CURSOR        RGB(59, 130, 246)
#define COLOR_TOOLBAR_BG    RGB(30, 58, 138)
#define COLOR_MENU_BG       RGB(30, 41, 59)
#define COLOR_MINIMAP_BG    RGB(20, 25, 35)

#define MAX_POINTER_DEVICES 8
#define MAX_TEXT            4096
#define MAX_FILES           32
#define PAINT_W             250
#define PAINT_H             180
#define ANIM_W              100
#define ANIM_H              70
#define MAX_ANIM_FRAMES     8
#define TERM_LINES          14
#define TERM_COLS           50
#define TOTAL_WINDOWS       26

#define TASKBAR_H            32
#define TASKBAR_START_W      62
#define TASKBAR_SCROLL_W     24
#define TASKBAR_BUTTON_W     62
#define TASKBAR_BUTTON_H     24
#define TASKBAR_CLOCK_W      78
#define TASKBAR_POWER_W      64
#define TASKBAR_GAP          4

#define SNAKE_GRID_W        20
#define SNAKE_GRID_H        15
#define SNAKE_CELL_SZ       12
#define SNAKE_MAX_LEN       (SNAKE_GRID_W * SNAKE_GRID_H)

typedef enum {
    TOOL_PENCIL, TOOL_BRUSH, TOOL_SQUARE, TOOL_CIRCLE,
    TOOL_TRIANGLE, TOOL_TEXT, TOOL_PAN, TOOL_MOVE
} PAINT_TOOL;

typedef struct UEFI_WINDOW UEFI_WINDOW;
typedef VOID (*WIN_RENDER_FUNC)(UEFI_WINDOW *Win);
typedef BOOLEAN (*WIN_INPUT_FUNC)(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN LeftClick, BOOLEAN LastClick);

struct UEFI_WINDOW {
    INT32 X, Y, W, H;
    INT32 OldX, OldY, OldW, OldH;
    BOOLEAN IsMaximized;
    BOOLEAN IsMinimized;
    BOOLEAN IsVisible;
    BOOLEAN IsDragging;
    INT32 DragOffsetX;
    INT32 DragOffsetY;
    CHAR16 Title[64];
    WIN_RENDER_FUNC Render;
    WIN_INPUT_FUNC Input;
};

typedef enum { EDITOR_MENU_NONE, EDITOR_MENU_FILE, EDITOR_MENU_EDIT, EDITOR_MENU_VIEW } EDITOR_MENU_STATE;

// --- Global Context ---
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *gGop = NULL;
STATIC UINT32 *gFrameBuffer = NULL;
STATIC UINT32 *gBackBuffer = NULL;
STATIC UINT32 gScreenWidth = 1024, gScreenHeight = 768, gPixelsPerScanLine = 1024;

STATIC EFI_SIMPLE_POINTER_PROTOCOL *gMouseDevices[MAX_POINTER_DEVICES];
STATIC UINTN gNumMouseDevices = 0;
STATIC EFI_ABSOLUTE_POINTER_PROTOCOL *gAbsMouse = NULL;
STATIC INT32 gMouseX = 400, gMouseY = 300;

STATIC BOOLEAN gStartMenuOpen = FALSE;
STATIC BOOLEAN gShowPowerDialog = FALSE;
STATIC BOOLEAN gInShellMode = FALSE;

STATIC CHAR16 gShellCmdBuf[128] = L"";
STATIC UINTN gShellCmdLen = 0;
STATIC CHAR16 gShellLog[20][80];
STATIC UINTN gShellLogCount = 0;

STATIC BOOLEAN gInTextTermEditor = FALSE;
STATIC CHAR16 gTextTermFileName[64] = L"untitled";
STATIC CHAR16 gTextTermBuffer[MAX_TEXT] = L"";
STATIC UINTN gTextTermLen = 0;
STATIC CHAR16 gTextTermStatus[64] = L"Editing file...";

STATIC CHAR16 gTermCmdBuf[128] = L"";
STATIC UINTN gTermCmdLen = 0;

STATIC CHAR16 gCalcDisplay[32] = L"0";
STATIC INT32 gCalcVal1 = 0;
STATIC CHAR16 gCalcOp = L'\0';
STATIC BOOLEAN gCalcNewEntry = TRUE;

typedef struct { INT32 X, Y; } POINT;
STATIC POINT gSnakeBody[SNAKE_MAX_LEN];
STATIC UINTN gSnakeLength = 3;
STATIC INT32 gSnakeDirX = 1, gSnakeDirY = 0;
STATIC POINT gSnakeFood = {10, 7};
STATIC BOOLEAN gSnakeGameOver = FALSE;
STATIC UINT32 gSnakeScore = 0;
STATIC UINTN gSnakeTick = 0;

STATIC CHAR16 gTextBuffer[MAX_TEXT];
STATIC UINTN gTextLen = 0;
STATIC BOOLEAN gWordWrap = TRUE, gShowMinimap = FALSE;
STATIC EDITOR_MENU_STATE gEditorMenuState = EDITOR_MENU_NONE;

STATIC CHAR16 gFindBuf[32] = L"", gReplaceBuf[32] = L"";
STATIC UINTN gFindLen = 0, gReplaceLen = 0;
STATIC INT32 gActiveField = 0;

STATIC UINT32 gPaintCanvas[PAINT_W * PAINT_H];
STATIC UINT32 gCurrentColor = RGB(255, 255, 255);
STATIC PAINT_TOOL gCurrentTool = TOOL_BRUSH;
STATIC INT32 gPanOffsetX = 0, gPanOffsetY = 0;
STATIC INT32 gPanLastX = 0, gPanLastY = 0;
STATIC CHAR16 gPaintTextBuf[32] = L"";
STATIC UINTN gPaintTextLen = 0;
STATIC INT32 gPaintTextTargetX = -1, gPaintTextTargetY = -1;
STATIC BOOLEAN gPaintTypingText = FALSE;

STATIC UINT32 g16Palette[16] = {
    RGB(0,0,0), RGB(0,0,170), RGB(0,170,0), RGB(0,170,170),
    RGB(170,0,0), RGB(170,0,170), RGB(170,85,0), RGB(170,170,170),
    RGB(85,85,85), RGB(85,85,255), RGB(85,255,85), RGB(85,255,255),
    RGB(255,85,85), RGB(255,85,255), RGB(255,255,85), RGB(255,255,255)
};

STATIC UINT32 gAnimFrames[MAX_ANIM_FRAMES][ANIM_W * ANIM_H];
STATIC UINTN gAnimFrameCount = 4, gEditFrameIdx = 0, gViewFrameIdx = 0;
STATIC BOOLEAN gAnimPlaying = FALSE;
STATIC UINTN gAnimSpeedDelay = 8, gAnimTickCount = 0;
STATIC UINT32 gAnimColor = RGB(255,255,255);

STATIC CONST CHAR16 *gJumpscareHexMessage[] = {
    L"6865792e2e206966207572652068657265207468656e",
    L"20692077616e7420746f206b6e6f7720746861742074",
    L"686973206a756d707363617265206973206465736967",
    L"6e656420666f722066756e2120627574206974732062",
    L"61642069206b6e6f772062757420692077616e742074",
    L"6f206b6e6f77207468617420746865726573206d6f72",
    L"65207468696e677320752063616e206d6f6469667920",
    L"696e2074686520636f646520626320697473206f7065",
    L"6e203b44"
};

STATIC CHAR16 gFileList[MAX_FILES][64];
STATIC UINTN gFileCount = 0;
STATIC INT32 gSelectedFileIndex = -1;

STATIC CHAR16 gTermBuffer[TERM_LINES][TERM_COLS];
STATIC UINTN gTermLineCount = 0;

// --- New Apps ---
#define NEW_APP_COLOR_BG       RGB(24,31,45)
#define NEW_APP_COLOR_PANEL    RGB(30,41,59)
#define NEW_APP_COLOR_ACCENT   RGB(59,130,246)
#define NEW_APP_COLOR_GOOD     RGB(34,197,94)
#define NEW_APP_COLOR_WARN     RGB(234,179,8)
#define NEW_APP_COLOR_BAD      RGB(225,29,72)
// ============================================================
// NOTES
// ============================================================

#define NOTES_MAX_TEXT 8192

STATIC CHAR16 gNotesBuffer[NOTES_MAX_TEXT];
STATIC UINTN gNotesLen = 0;
STATIC CHAR16 gNotesFileName[64] = L"Notes.txt";
STATIC BOOLEAN gNotesDirty = FALSE;

// ============================================================
// MINI IDE
// ============================================================

#define IDE_MAX_TEXT 12288

STATIC CHAR16 gIDEBuffer[IDE_MAX_TEXT];
STATIC UINTN gIDELen = 0;
STATIC CHAR16 gIDEFileName[64] = L"main.c";
STATIC UINTN gIDECursor = 0;
STATIC BOOLEAN gIDEDirty = FALSE;

// ============================================================
// TETRIS
// ============================================================

#define TETRIS_W 10
#define TETRIS_H 20

STATIC UINT8 gTetrisBoard[TETRIS_H][TETRIS_W];

STATIC INT32 gTetrisX = 3;
STATIC INT32 gTetrisY = 0;

STATIC UINT8 gTetrisPiece = 0;
STATIC UINT8 gTetrisNextPiece = 1;
STATIC UINT8 gTetrisRotation = 0;

STATIC UINT32 gTetrisScore = 0;
STATIC UINT32 gTetrisLines = 0;
STATIC UINT32 gTetrisLevel = 1;

// Real-time Tetris timer.
// This stores the last second when the piece moved.
STATIC UINT8 gTetrisLastSecond = 0;

STATIC BOOLEAN gTetrisGameOver = FALSE;
STATIC UINT32 gTetrisRandomState = 0x13579BDF;

// ============================================================
// COMMON APPS
// ============================================================

typedef enum {
    COMMON_APP_CALENDAR,
    COMMON_APP_CLOCK,
    COMMON_APP_STOPWATCH,
    COMMON_APP_ABOUT
} COMMON_APP_TAB;

STATIC COMMON_APP_TAB gCommonAppTab = COMMON_APP_CALENDAR;

// Calendar state
STATIC BOOLEAN gCalendarInitialized = FALSE;
STATIC UINT16 gCalendarYear = 2026;
STATIC UINT8 gCalendarMonth = 1;
STATIC UINT8 gCalendarSelectedDay = 1;

// Stopwatch state
STATIC BOOLEAN gStopwatchRunning = FALSE;
STATIC UINT32 gStopwatchTicks = 0;
STATIC UINT8 gStopwatchLastSecond = 0;

STATIC UINT32 gWallpaperColor1 = RGB(30,41,59);
STATIC UINT32 gWallpaperColor2 = RGB(15,23,42);
STATIC UINTN gWallpaperMode = 0;
STATIC BOOLEAN gWallpaperAnimate = FALSE;
STATIC UINT32 gWallpaperPhase = 0;

STATIC CHAR16 gRoboInput[96] = L"";
STATIC UINTN gRoboInputLen = 0;
STATIC CHAR16 gRoboLog[10][96];
STATIC UINTN gRoboLogCount = 0;
STATIC INT32 gTaskMgrSelected = -1;
// ============================================================
// JUMPSCARE
// ============================================================

STATIC BOOLEAN gJumpscareActive = FALSE;
STATIC UINT32 gJumpscareElapsedMs = 0;
STATIC UINT32 gJumpscareRandom = 0xA53C91E7;
STATIC BOOLEAN gJumpscareSoundPending = FALSE;
STATIC BOOLEAN gJumpscareMessage = FALSE;
STATIC UINT32 gJumpscareMessageTimer = 0;

// ============================================================
// TASKBAR STATE
// ============================================================

STATIC UINTN gTaskbarScroll = 0;

// --- Forward Declarations ---
VOID ShowStartupScreen(VOID);
VOID ShowShutdownSequence(BOOLEAN IsReboot);
VOID TerminalPrint(IN CONST CHAR16 *Text);
VOID ExecuteCommandString(IN CONST CHAR16 *Cmd, IN BOOLEAN IsShell);
EFI_STATUS SaveFileToFS(IN CHAR16 *FileName, IN VOID *Buffer, IN UINTN BufferSize);
EFI_STATUS LoadFileFromFS(IN CHAR16 *FileName, IN VOID *Buffer, IN OUT UINTN *BufferSize);
VOID BringToFront(UEFI_WINDOW *Win);

VOID RenderEditor(UEFI_WINDOW *Win); BOOLEAN InputEditor(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderPaint(UEFI_WINDOW *Win); BOOLEAN InputPaint(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderTerminal(UEFI_WINDOW *Win); BOOLEAN InputTerminal(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderFileManager(UEFI_WINDOW *Win); BOOLEAN InputFileManager(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderFindReplace(UEFI_WINDOW *Win); BOOLEAN InputFindReplace(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderAnimEdit(UEFI_WINDOW *Win); BOOLEAN InputAnimEdit(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderAnimView(UEFI_WINDOW *Win); BOOLEAN InputAnimView(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderCalc(UEFI_WINDOW *Win); BOOLEAN InputCalc(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSysInfo(UEFI_WINDOW *Win); BOOLEAN InputSysInfo(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSnake(UEFI_WINDOW *Win); BOOLEAN InputSnake(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderTextTerm(UEFI_WINDOW *Win); BOOLEAN InputTextTerm(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderDiskUtil(UEFI_WINDOW *Win); BOOLEAN InputDiskUtil(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);

VOID RenderWallpaper(UEFI_WINDOW *Win);
BOOLEAN InputWallpaper(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderRoboChat(UEFI_WINDOW *Win);
BOOLEAN InputRoboChat(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderTaskManager(UEFI_WINDOW *Win);
BOOLEAN InputTaskManager(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID PollNewAppsKeyboard(VOID);
VOID RenderNewWallpaper(VOID);

// ============================================================
// NEW APP FORWARD DECLARATIONS
// ============================================================

VOID RenderNotes(UEFI_WINDOW *Win);
BOOLEAN InputNotes(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderMiniIDE(UEFI_WINDOW *Win);
BOOLEAN InputMiniIDE(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderTetris(UEFI_WINDOW *Win);
BOOLEAN InputTetris(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderCommonApps(UEFI_WINDOW *Win);
BOOLEAN InputCommonApps(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

// ============================================================
// IMAGE VIEWER
// ============================================================

#define IMAGE_VIEW_W 250
#define IMAGE_VIEW_H 180

STATIC UINT32 gImageViewerCanvas[PAINT_W * PAINT_H];
STATIC CHAR16 gImageViewerFileName[64] = L"Image.pnt";
STATIC BOOLEAN gImageViewerLoaded = FALSE;
STATIC INT32 gImageViewerZoom = 1;

// ============================================================
// FILE ARCHIVE
// ============================================================

#define UARCH_MAX_FILES 8
#define UARCH_NAME_LEN  64
#define UARCH_MAGIC     0x55415243u

typedef struct {
    UINT32 Magic;
    UINT32 FileCount;
} UARCH_HEADER;

typedef struct {
    CHAR16 FileName[UARCH_NAME_LEN];
    UINT32 FileSize;
} UARCH_ENTRY;

STATIC CHAR16 gArchiveName[64] = L"Bundle.uarch";
STATIC BOOLEAN gArchiveStatusGood = FALSE;

// ============================================================
// CLIPBOARD
// ============================================================

#define CLIPBOARD_HISTORY 8
#define CLIPBOARD_TEXT_MAX 1024

STATIC CHAR16 gClipboardHistory[
    CLIPBOARD_HISTORY
][CLIPBOARD_TEXT_MAX];

STATIC UINTN gClipboardCount = 0;
STATIC INT32 gClipboardSelected = -1;

// ============================================================
// UNIT CONVERTER
// ============================================================

typedef enum {
    UNIT_BYTES,
    UNIT_TEMPERATURE
} UNIT_MODE;

STATIC UNIT_MODE gUnitMode = UNIT_BYTES;
STATIC CHAR16 gUnitInput[32] = L"1";
STATIC UINTN gUnitInputLen = 1;

// ============================================================
// FILE SEARCH
// ============================================================

STATIC CHAR16 gSearchQuery[64] = L"";
STATIC UINTN gSearchQueryLen = 0;
STATIC CHAR16 gSearchResults[MAX_FILES][64];
STATIC UINTN gSearchResultCount = 0;
STATIC BOOLEAN gSearchDone = FALSE;

// ============================================================
// PC SPEAKER MUSIC
// ============================================================

STATIC BOOLEAN gMusicPlaying = FALSE;
// STATIC UINTN gMusicSong = 0; bc unused error!

// ============================================================
// SECRET EASTER EGG
// ============================================================

STATIC CHAR16 gEasterSequence[] =
    L"uefitext1337";

STATIC UINTN gEasterSequencePos = 0;
STATIC BOOLEAN gEasterEggUnlocked = FALSE;

VOID ResetTetris(VOID);
VOID UpdateTetris(VOID);

VOID RenderImageViewer(UEFI_WINDOW *Win);
BOOLEAN InputImageViewer(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderArchive(UEFI_WINDOW *Win);
BOOLEAN InputArchive(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderClipboard(UEFI_WINDOW *Win);
BOOLEAN InputClipboard(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderUnitConverter(UEFI_WINDOW *Win);
BOOLEAN InputUnitConverter(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderSearch(UEFI_WINDOW *Win);
BOOLEAN InputSearch(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderMusic(UEFI_WINDOW *Win);
BOOLEAN InputMusic(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

VOID RenderEasterEgg(UEFI_WINDOW *Win);
BOOLEAN InputEasterEgg(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);

// --- Windows ---
STATIC UEFI_WINDOW gEditorWin   = {40,40,580,420,40,40,580,420,FALSE,FALSE,FALSE,FALSE,0,0,L"Text Editor",RenderEditor,InputEditor};
STATIC UEFI_WINDOW gPaintWin    = {180,50,480,420,180,50,480,420,FALSE,FALSE,FALSE,FALSE,0,0,L"Paint Studio",RenderPaint,InputPaint};
STATIC UEFI_WINDOW gTermWin     = {120,140,460,280,120,140,460,280,FALSE,FALSE,FALSE,FALSE,0,0,L"Terminal Console",RenderTerminal,InputTerminal};
STATIC UEFI_WINDOW gFileWin     = {260,100,380,300,260,100,380,300,FALSE,FALSE,FALSE,FALSE,0,0,L"File Manager",RenderFileManager,InputFileManager};
STATIC UEFI_WINDOW gFindWin     = {200,150,320,140,200,150,320,140,FALSE,FALSE,FALSE,FALSE,0,0,L"Find & Replace",RenderFindReplace,InputFindReplace};
STATIC UEFI_WINDOW gAnimEditWin = {100,60,400,360,100,60,400,360,FALSE,FALSE,FALSE,FALSE,0,0,L"Animation Editor",RenderAnimEdit,InputAnimEdit};
STATIC UEFI_WINDOW gAnimViewWin = {520,80,280,260,520,80,280,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Animation Viewer",RenderAnimView,InputAnimView};
STATIC UEFI_WINDOW gCalcWin     = {300,180,220,260,300,180,220,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Calculator",RenderCalc,InputCalc};
STATIC UEFI_WINDOW gSysInfoWin  = {150,100,360,220,150,100,360,220,FALSE,FALSE,FALSE,FALSE,0,0,L"System Info",RenderSysInfo,InputSysInfo};
STATIC UEFI_WINDOW gSnakeWin    = {180,120,280,280,180,120,280,280,FALSE,FALSE,FALSE,FALSE,0,0,L"Snake Game",RenderSnake,InputSnake};
STATIC UEFI_WINDOW gTextTermWin = {80,80,480,300,80,80,480,300,FALSE,FALSE,FALSE,FALSE,0,0,L"TextTerm Shell",RenderTextTerm,InputTextTerm};
STATIC UEFI_WINDOW gDiskUtilWin = {240,110,420,260,240,110,420,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Disk Utility",RenderDiskUtil,InputDiskUtil};

STATIC UEFI_WINDOW gWallpaperWin = {
    90,90,390,270,
    90,90,390,270,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Wallpaper",
    RenderWallpaper,
    InputWallpaper
};

STATIC UEFI_WINDOW gRoboChatWin = {
    230,120,440,330,
    230,120,440,330,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"RoboChat",
    RenderRoboChat,
    InputRoboChat
};

STATIC UEFI_WINDOW gTaskMgrWin = {
    170,60,520,410,
    170,60,520,410,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Task Manager",
    RenderTaskManager,
    InputTaskManager
};

// ============================================================
// NEW WINDOWS
// ============================================================

STATIC UEFI_WINDOW gNotesWin = {
    120,70,520,380,
    120,70,520,380,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Notes",
    RenderNotes,
    InputNotes
};

STATIC UEFI_WINDOW gMiniIDEWin = {
    70,45,720,500,
    70,45,720,500,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Mini IDE",
    RenderMiniIDE,
    InputMiniIDE
};

STATIC UEFI_WINDOW gTetrisWin = {
    170,60,390,520,
    170,60,390,520,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Tetris",
    RenderTetris,
    InputTetris
};

STATIC UEFI_WINDOW gCommonAppsWin = {
    210,120,500,320,
    210,120,500,320,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Common Apps",
    RenderCommonApps,
    InputCommonApps
};

// ============================================================
// NEW WINDOWS
// ============================================================

STATIC UEFI_WINDOW gImageViewerWin = {
    160,80,420,320,
    160,80,420,320,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Image Viewer",
    RenderImageViewer,
    InputImageViewer
};

STATIC UEFI_WINDOW gArchiveWin = {
    180,100,460,300,
    180,100,460,300,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"File Archive",
    RenderArchive,
    InputArchive
};

STATIC UEFI_WINDOW gClipboardWin = {
    190,90,500,330,
    190,90,500,330,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Clipboard",
    RenderClipboard,
    InputClipboard
};

STATIC UEFI_WINDOW gUnitConverterWin = {
    220,120,400,280,
    220,120,400,280,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Unit Converter",
    RenderUnitConverter,
    InputUnitConverter
};

STATIC UEFI_WINDOW gSearchWin = {
    150,90,560,360,
    150,90,560,360,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"Search",
    RenderSearch,
    InputSearch
};

STATIC UEFI_WINDOW gMusicWin = {
    220,130,400,250,
    220,130,400,250,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"PC Speaker Music",
    RenderMusic,
    InputMusic
};

STATIC UEFI_WINDOW gEasterEggWin = {
    300,180,400,250,
    300,180,400,250,
    FALSE,FALSE,FALSE,FALSE,
    0,0,
    L"what in the world did u came from?",
    RenderEasterEgg,
    InputEasterEgg
};

STATIC UEFI_WINDOW *gWinOrder[TOTAL_WINDOWS] = {
    &gEditorWin,
    &gPaintWin,
    &gTermWin,
    &gFileWin,
    &gFindWin,
    &gAnimEditWin,
    &gAnimViewWin,
    &gCalcWin,
    &gSysInfoWin,
    &gSnakeWin,
    &gTextTermWin,
    &gDiskUtilWin,
    &gWallpaperWin,
    &gRoboChatWin,
    &gTaskMgrWin,

    &gNotesWin,
    &gMiniIDEWin,
    &gTetrisWin,
    &gCommonAppsWin,

    &gImageViewerWin,
    &gArchiveWin,
    &gClipboardWin,
    &gUnitConverterWin,
    &gSearchWin,
    &gMusicWin,
    &gEasterEggWin
};

// ============================================================
// MASTER TASKBAR WINDOW LIST
// ============================================================

STATIC UEFI_WINDOW *gTaskbarWindows[] = {

    &gEditorWin,
    &gPaintWin,
    &gTermWin,
    &gFileWin,
    &gAnimEditWin,
    &gAnimViewWin,
    &gCalcWin,
    &gSysInfoWin,
    &gSnakeWin,
    &gTextTermWin,
    &gDiskUtilWin,

    &gWallpaperWin,
    &gRoboChatWin,
    &gTaskMgrWin,

    &gNotesWin,
    &gMiniIDEWin,
    &gTetrisWin,
    &gCommonAppsWin,

    &gImageViewerWin,
    &gArchiveWin,
    &gClipboardWin,
    &gUnitConverterWin,
    &gSearchWin,
    &gMusicWin
};

#define TASKBAR_WINDOW_COUNT \
    (sizeof(gTaskbarWindows) / sizeof(gTaskbarWindows[0]))
    
STATIC CONST CHAR16 *gTaskbarNames[] = {

    L"Editor",
    L"Paint",
    L"Term",
    L"Files",
    L"AnimE",
    L"AnimV",
    L"Calc",
    L"SysInfo",
    L"Snake",
    L"Shell",
    L"Disk",

    L"Wall",
    L"Robo",
    L"TaskMgr",

    L"Notes",
    L"IDE",
    L"Tetris",
    L"Common",

    L"ImgView",
    L"Archive",
    L"Clipboard",
    L"Convert",
    L"Search",
    L"Music"
};

STATIC UINT32 JumpscareRandom(VOID) {
    gJumpscareRandom =
        gJumpscareRandom * 1664525u +
        1013904223u;

    return gJumpscareRandom;
}

STATIC UINTN GetTaskbarVisibleCount(VOID) {

    INT32 AvailableWidth =
        (INT32)gScreenWidth
        - TASKBAR_START_W
        - TASKBAR_SCROLL_W * 2
        - TASKBAR_CLOCK_W
        - TASKBAR_POWER_W
        - 20;

    if (AvailableWidth <= 0)
        return 1;

    UINTN Count =
        (UINTN)(AvailableWidth / TASKBAR_BUTTON_W);

    if (Count == 0)
        Count = 1;

    return Count;
}

// --- Assets ---
STATIC CONST UINT8 MOUSE_CURSOR_16x16[16][16] = {
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,1,1,1,0,0,0,0,0,0},
    {1,2,2,1,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,1,2,2,1,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,1,2,2,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,1,2,2,1,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,1,1,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}
};

STATIC CONST UINT8 FONT_8x8[128][8] = {
    [' ']={0,0,0,0,0,0,0,0},['!']={0x18,0x18,0x18,0x18,0x18,0,0x18,0},
    ['"']={0x6C,0x6C,0x6C,0,0,0,0,0},['#']={0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0},
    ['$']={0x18,0x3E,0x60,0x3C,6,0x7C,0x18,0},['%']={0,0xC6,0xCC,0x18,0x30,0x66,0xC6,0},
    ['&']={0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0},['\'']={0x18,0x18,0x30,0,0,0,0,0},
    ['(']={0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0},[')']={0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0},
    ['*']={0,0x66,0x3C,0xFF,0x3C,0x66,0,0},['+']={0,0x18,0x18,0x7E,0x18,0x18,0,0},
    [',']={0,0,0,0,0,0x18,0x18,0x30},['-']={0,0,0,0x7E,0,0,0,0},
    ['.']={0,0,0,0,0,0x18,0x18,0},['/']={0,6,0x0C,0x18,0x30,0x60,0,0},
    ['0']={0x3C,0x66,0x6E,0x76,0x66,0x66,0x3C,0},['1']={0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0},
    ['2']={0x3C,0x66,6,0x1C,0x30,0x60,0x7E,0},['3']={0x3C,0x66,6,0x1C,6,0x66,0x3C,0},
    ['4']={0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0},['5']={0x7E,0x60,0x7C,6,6,0x66,0x3C,0},
    ['6']={0x3C,0x60,0x7C,0x66,0x66,0x66,0x3C,0},['7']={0x7E,6,0x0C,0x18,0x30,0x30,0x30,0},
    ['8']={0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0},['9']={0x3C,0x66,0x66,0x3E,6,0x0C,0x38,0},
    [':']={0,0x18,0x18,0,0,0x18,0x18,0},[';']={0,0x18,0x18,0,0,0x18,0x18,0x30},
    ['<']={6,0x0C,0x18,0x30,0x18,0x0C,6,0},['=']={0,0,0x7E,0,0,0x7E,0,0},
    ['>']={0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0},['?']={0x3C,0x66,6,0x0C,0x18,0,0x18,0},
    ['@']={0x7C,0x82,0x9A,0xAA,0xAA,0x9E,0x7C,0},
    ['A']={0x18,0x3C,0x66,0x66,0x7E,0x66,0x66,0},['B']={0x7C,0x66,0x66,0x7C,0x66,0x66,0x7C,0},
    ['C']={0x3C,0x66,0x60,0x60,0x60,0x66,0x3C,0},['D']={0x78,0x6C,0x66,0x66,0x66,0x6C,0x78,0},
    ['E']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x7E,0},['F']={0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0},
    ['G']={0x3C,0x66,0x60,0x6E,0x66,0x66,0x3C,0},['H']={0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0},
    ['I']={0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0},['J']={6,6,6,6,6,0x66,0x3C,0},
    ['K']={0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0},['L']={0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0},
    ['M']={0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0},['N']={0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0},
    ['O']={0x3C,0x66,0x66,0x66,0x66,0x66,0x3C,0},['P']={0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0},
    ['Q']={0x3C,0x66,0x66,0x66,0x6A,0x6C,0x36,0},['R']={0x7C,0x66,0x66,0x7C,0x6C,0x66,0x66,0},
    ['S']={0x3C,0x66,0x60,0x3C,6,0x66,0x3C,0},['T']={0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0},
    ['U']={0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0},['V']={0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0},
    ['W']={0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0},['X']={0x66,0x66,0x3C,0x18,0x3C,0x66,0x66,0},
    ['Y']={0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0},['Z']={0x7E,6,0x0C,0x18,0x30,0x60,0x7E,0},
    ['[']={0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0},['\\']={0,0x60,0x30,0x18,0x0C,6,0,0},
    [']']={0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0},['^']={0x18,0x3C,0x66,0,0,0,0,0},
    ['_']={0,0,0,0,0,0,0xFF,0},['`']={0x30,0x18,0x0C,0,0,0,0,0},
    ['a']={0,0,0x3C,6,0x3E,0x66,0x3E,0},['b']={0x60,0x60,0x7C,0x66,0x66,0x66,0x7C,0},
    ['c']={0,0,0x3C,0x60,0x60,0x60,0x3C,0},['d']={6,6,0x3E,0x66,0x66,0x66,0x3E,0},
    ['e']={0,0,0x3C,0x66,0x7E,0x60,0x3C,0},['f']={0x1C,0x30,0x7C,0x30,0x30,0x30,0x30,0},
    ['g']={0,0,0x3E,0x66,0x66,0x3E,6,0x3C},['h']={0x60,0x60,0x7C,0x66,0x66,0x66,0x66,0},
    ['i']={0x18,0,0x38,0x18,0x18,0x18,0x3C,0},['j']={0x0C,0,0x1C,0x0C,0x0C,0x0C,0x0C,0x38},
    ['k']={0x60,0x60,0x66,0x6C,0x78,0x6C,0x66,0},['l']={0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0},
    ['m']={0,0,0x6C,0x7E,0x54,0x54,0x54,0},['n']={0,0,0x7C,0x66,0x66,0x66,0x66,0},
    ['o']={0,0,0x3C,0x66,0x66,0x66,0x3C,0},['p']={0,0,0x7C,0x66,0x66,0x7C,0x60,0x60},
    ['q']={0,0,0x3E,0x66,0x66,0x3E,6,6},['r']={0,0,0x5C,0x66,0x60,0x60,0x60,0},
    ['s']={0,0,0x3E,0x60,0x3C,6,0x7C,0},['t']={0x30,0x30,0x7C,0x30,0x30,0x30,0x1C,0},
    ['u']={0,0,0x66,0x66,0x66,0x66,0x3E,0},['v']={0,0,0x66,0x66,0x66,0x3C,0x18,0},
    ['w']={0,0,0x63,0x6B,0x7F,0x3E,0x36,0},['x']={0,0,0x66,0x3C,0x18,0x3C,0x66,0},
    ['y']={0,0,0x66,0x66,0x66,0x3E,0x0C,0x78},['z']={0,0,0x7E,0x0C,0x18,0x30,0x7E,0},
    ['{']={0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0},['|']={0x18,0x18,0x18,0,0x18,0x18,0x18,0},
    ['}']={0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0},['~']={0x76,0xDC,0,0,0,0,0,0}
};

// --- Geometry / Drawing ---
STATIC BOOLEAN IsInRect(INT32 X, INT32 Y, INT32 RX, INT32 RY, INT32 RW, INT32 RH) {
    return (X >= RX && X < RX + RW && Y >= RY && Y < RY + RH);
}

STATIC VOID FastDrawPixel(INT32 x, INT32 y, UINT32 color) {
    if (x >= 0 && y >= 0 && x < (INT32)gScreenWidth && y < (INT32)gScreenHeight)
        gBackBuffer[y * gPixelsPerScanLine + x] = color;
}

VOID DrawRect(INT32 x, INT32 y, INT32 w, INT32 h, UINT32 color) {
    for (INT32 r = y; r < y + h; r++)
        for (INT32 c = x; c < x + w; c++) FastDrawPixel(c, r, color);
}

VOID DrawChar(CHAR16 ch, INT32 x, INT32 y, UINT32 color) {
    if (ch > 127) ch = '?';
    for (INT32 r = 0; r < 8; r++) {
        UINT8 row = FONT_8x8[ch][r];
        for (INT32 c = 0; c < 8; c++)
            if (row & (1 << (7 - c))) FastDrawPixel(x + c, y + r, color);
    }
}

VOID DrawString(IN CONST CHAR16 *str, INT32 x, INT32 y, UINT32 color) {
    while (*str) { DrawChar(*str, x, y, color); x += 8; str++; }
}

VOID DrawButton(INT32 x, INT32 y, INT32 w, INT32 h, CONST CHAR16 *label, UINT32 bg, UINT32 fg) {
    DrawRect(x, y, w, h, bg);
    DrawString(label, x + (w - (INT32)StrLen(label) * 8) / 2, y + (h - 8) / 2, fg);
}

VOID SwapBuffers(VOID) {
    if (gBackBuffer != gFrameBuffer)
        gBS->CopyMem(gFrameBuffer, gBackBuffer, gPixelsPerScanLine * gScreenHeight * sizeof(UINT32));
}

// --- PC Speaker ---
VOID PlayTone(UINT32 FreqHz, UINT32 DurationMs) {
    if (FreqHz == 0) {
        gBS->Stall(DurationMs * 1000);
        return;
    }

    UINT32 Divisor = 1193182 / FreqHz;

    io_outb(0x43, 0xB6);
    io_outb(0x42, (UINT8)(Divisor & 0xFF));
    io_outb(0x42, (UINT8)((Divisor >> 8) & 0xFF));

    UINT8 State = io_inb(0x61);
    if ((State & 0x03) != 0x03) io_outb(0x61, State | 0x03);

    gBS->Stall(DurationMs * 1000);
    io_outb(0x61, io_inb(0x61) & ~0x03);
}

VOID PlayStartupJingle(VOID) {
    PlayTone(523,100);
    PlayTone(659,100);
    PlayTone(784,100);
    PlayTone(1047,300);
}

VOID PlayShutdownJingle(VOID) {
    PlayTone(784,120);
    PlayTone(659,120);
    PlayTone(523,120);
    PlayTone(392,300);
}

// --- Startup / Shutdown ---
VOID ShowStartupScreen(VOID) {
    INT32 CenterX = (INT32)gScreenWidth / 2;
    INT32 CenterY = (INT32)gScreenHeight / 2;

    for (INT32 step = 0; step <= 100; step += 4) {
        DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(15,23,42));

        DrawString(L"=================================",CenterX-132,CenterY-60,RGB(59,130,246));
        DrawString(L" Welcome to my Uefitext program! ",CenterX-132,CenterY-40,COLOR_TEXT);
        DrawString(L"=================================",CenterX-132,CenterY-20,RGB(59,130,246));

        DrawRect(CenterX-120,CenterY+20,240,18,RGB(51,65,85));
        DrawRect(CenterX-118,CenterY+22,(236*step)/100,14,RGB(37,99,235));

        DrawString(L"Loading plz wait ._.",CenterX-140,CenterY+50,RGB(148,163,184));

        SwapBuffers();
        gBS->Stall(25000);
    }

    PlayStartupJingle();
}

VOID ShowShutdownSequence(BOOLEAN IsReboot) {
    INT32 CenterX = (INT32)gScreenWidth / 2;
    INT32 CenterY = (INT32)gScreenHeight / 2;

    DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(15,23,42));
    DrawString(L"Shutting down...",CenterX-112,CenterY-10,COLOR_TEXT);
    DrawString(L"Doing stuff to close and power off!",CenterX-192,CenterY+10,RGB(148,163,184));
    SwapBuffers();
    gBS->Stall(1200000);

    UINT32 AmberColor = RGB(245,158,11);

    for (INT32 seconds = 10; seconds >= 0; seconds--) {
        DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));

        DrawString(L"It is now safe to turn off",CenterX-104,CenterY-30,AmberColor);
        DrawString(L"your computer. :D",CenterX-56,CenterY-10,AmberColor);

        CHAR16 TimerMsg[64];
        if (IsReboot)
            UnicodeSPrint(TimerMsg,sizeof(TimerMsg),L"Rebooting in %d seconds...",seconds);
        else
            UnicodeSPrint(TimerMsg,sizeof(TimerMsg),L"Powering off in %d seconds...",seconds);

        DrawString(TimerMsg,CenterX-((INT32)StrLen(TimerMsg)*4),CenterY+40,RGB(100,116,139));
        SwapBuffers();

        if (seconds > 0) {
            PlayTone(880,50);
            gBS->Stall(950000);
        } else {
            gBS->Stall(1000000);
        }
    }

    PlayShutdownJingle();
    gRT->ResetSystem(IsReboot ? EfiResetCold : EfiResetShutdown,EFI_SUCCESS,0,NULL);
}

// --- Shell ---
VOID ShellPrint(IN CONST CHAR16 *Msg) {
    if (gShellLogCount >= 20) {
        for (UINTN i = 0; i < 19; i++)
            gBS->CopyMem(gShellLog[i],gShellLog[i+1],80*sizeof(CHAR16));
        gShellLogCount = 19;
    }
    StrCpyS(gShellLog[gShellLogCount],80,Msg);
    gShellLogCount++;
}

VOID TerminalPrint(IN CONST CHAR16 *Text) {
    if (gTermLineCount >= TERM_LINES) {
        for (UINTN i=0;i<TERM_LINES-1;i++)
            gBS->CopyMem(gTermBuffer[i],gTermBuffer[i+1],TERM_COLS*sizeof(CHAR16));
        gTermLineCount = TERM_LINES-1;
    }

    UINTN i=0;
    while (Text[i] != L'\0' && i < TERM_COLS-1) {
        gTermBuffer[gTermLineCount][i] = Text[i];
        i++;
    }
    gTermBuffer[gTermLineCount][i] = L'\0';
    gTermLineCount++;
}

VOID RefreshFileList(VOID) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS=NULL;
    EFI_FILE_PROTOCOL *Root=NULL;
    gFileCount=0;

    if (EFI_ERROR(gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid,NULL,(VOID**)&FS)) ||
        EFI_ERROR(FS->OpenVolume(FS,&Root))) return;

    UINTN BufferSize=1024;
    EFI_FILE_INFO *FileInfo=NULL;

    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData,BufferSize,(VOID**)&FileInfo))) {
        Root->Close(Root);
        return;
    }

    Root->SetPosition(Root,0);
    while (gFileCount < MAX_FILES) {
        UINTN ReadSize=BufferSize;
        if (EFI_ERROR(Root->Read(Root,&ReadSize,FileInfo)) || ReadSize==0) break;

        if (!(FileInfo->Attribute & EFI_FILE_DIRECTORY)) {
            gBS->CopyMem(gFileList[gFileCount],FileInfo->FileName,64*sizeof(CHAR16));
            gFileList[gFileCount][63]=L'\0';
            gFileCount++;
        }
    }

    gBS->FreePool(FileInfo);
    Root->Close(Root);
}

VOID OpenTextTermEditor(IN CONST CHAR16 *FileName) {
    gInTextTermEditor=TRUE;

    if (StrLen(FileName)>0) {
        StrCpyS(gTextTermFileName,64,FileName);
        UINTN ReadSize=sizeof(gTextTermBuffer);

        if (EFI_ERROR(LoadFileFromFS((CHAR16*)FileName,gTextTermBuffer,&ReadSize))) {
            gTextTermBuffer[0]=L'\0';
            gTextTermLen=0;
            StrCpyS(gTextTermStatus,64,L"New file created.");
        } else {
            gTextTermLen=ReadSize/sizeof(CHAR16);
            if (gTextTermLen >= MAX_TEXT) gTextTermLen=MAX_TEXT-1;
            gTextTermBuffer[gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"File loaded successfully.");
        }
    } else {
        StrCpyS(gTextTermFileName,64,L"untitled");
        gTextTermBuffer[0]=L'\0';
        gTextTermLen=0;
        StrCpyS(gTextTermStatus,64,L"Opened untitled.");
    }
}

VOID SaveTextTermFile(VOID) {
    EFI_STATUS Status=SaveFileToFS(gTextTermFileName,gTextTermBuffer,gTextTermLen*sizeof(CHAR16));
    if (!EFI_ERROR(Status))
        UnicodeSPrint(gTextTermStatus,sizeof(gTextTermStatus),L"Saved to %s!",gTextTermFileName);
    else
        StrCpyS(gTextTermStatus,64,L"Error saving file!");
}

VOID ExecuteCommandString(IN CONST CHAR16 *Cmd, IN BOOLEAN IsShell) {
    VOID (*PrintFunc)(IN CONST CHAR16*) = IsShell ? ShellPrint : TerminalPrint;

    PrintFunc(Cmd);

    if (StrnCmp(Cmd,L"textterm",8)==0) {
        CONST CHAR16 *Arg=Cmd+8;
        while (*Arg==L' ') Arg++;
        OpenTextTermEditor(Arg);
        gTextTermWin.IsVisible=TRUE;
        gTextTermWin.IsMinimized=FALSE;
        BringToFront(&gTextTermWin);
    } else if (StrCmp(Cmd,L"gui")==0) {
        if (IsShell) gInShellMode=FALSE;
        else PrintFunc(L"GUI already running.");
    } else if (StrCmp(Cmd,L"poweroff")==0) {
        ShowShutdownSequence(FALSE);
    } else if (StrCmp(Cmd,L"reboot")==0) {
        ShowShutdownSequence(TRUE);
    } else if (StrCmp(Cmd,L"version")==0) {
        PrintFunc(L"My UefiText version is v1.0-x64)");
    } else if (StrCmp(Cmd,L"clear")==0) {
        if (IsShell) gShellLogCount=0;
        else gTermLineCount=0;
    } else if (StrCmp(Cmd,L"ls")==0) {
        RefreshFileList();
        if (gFileCount==0) PrintFunc(L"No files found.");
        else for (UINTN i=0;i<gFileCount;i++) PrintFunc(gFileList[i]);
    } else if (StrnCmp(Cmd,L"filemake ",9)==0) {
        CONST CHAR16 *FileName=Cmd+9;

        if (StrLen(FileName)>0) {
            CHAR16 DummyData=L'\0';
            EFI_STATUS Status=SaveFileToFS((CHAR16*)FileName,&DummyData,0);

            if (!EFI_ERROR(Status)) {
                PrintFunc(L"File created successfully.");
                RefreshFileList();
            } else {
                PrintFunc(L"Error: Failed to create file.");
            }
        } else {
            PrintFunc(L"Usage: filemake <filename>");
        }
    } else if (StrCmp(Cmd,L"help")==0) {
        PrintFunc(L"Cmds: textterm <file>, gui, reboot, poweroff,");
        PrintFunc(L"      version, ls, filemake <name>, clear, help");
    } else {
        PrintFunc(L"Unknown command. Type 'help'");
    }
}

VOID ExecuteShellCommand(VOID) {
    if (gShellCmdLen==0) return;
    ExecuteCommandString(gShellCmdBuf,TRUE);
    gShellCmdBuf[0]=L'\0';
    gShellCmdLen=0;
}

VOID ExecuteTermCommand(VOID) {
    if (gTermCmdLen==0) return;
    ExecuteCommandString(gTermCmdBuf,FALSE);
    gTermCmdBuf[0]=L'\0';
    gTermCmdLen=0;
}

VOID RenderShellMode(VOID) {
    DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));
    DrawString(L"Welcome to the Shell! :>",10,10,RGB(34,197,94));
    DrawString(L"Type 'textterm <file>' for text editor or 'gui' to switch desktop.",10,24,RGB(148,163,184));

    INT32 Y=50;
    for (UINTN i=0;i<gShellLogCount;i++) {
        DrawString(gShellLog[i],10,Y,RGB(241,245,249));
        Y+=14;
    }

    DrawString(L"> ",10,Y,RGB(34,197,94));
    DrawString(gShellCmdBuf,26,Y,RGB(255,255,255));
    DrawRect(26+(gShellCmdLen*8),Y,8,10,RGB(34,197,94));
}

// ============================================================
// AUTO-FIT UTILITY WINDOWS
// ============================================================

STATIC VOID KeepWindowOnScreen(UEFI_WINDOW *Win) {

    if (!Win || Win->IsMaximized)
        return;

    INT32 MaxBottom =
        (INT32)gScreenHeight - TASKBAR_H - 4;

    INT32 MaxRight =
        (INT32)gScreenWidth - 4;

    // Keep the window inside the left/top edges.
    if (Win->X < 4)
        Win->X = 4;

    if (Win->Y < 30)
        Win->Y = 30;

    // Keep the right/bottom edges inside the usable desktop.
    if (Win->X + Win->W > MaxRight)
        Win->X = MaxRight - Win->W;

    if (Win->Y + Win->H > MaxBottom)
        Win->Y = MaxBottom - Win->H;

    if (Win->X < 4)
        Win->X = 4;

    if (Win->Y < 30)
        Win->Y = 30;
}

STATIC VOID ResizeTaskManagerToFit(UEFI_WINDOW *Win) {

    if (!Win || Win->IsMaximized)
        return;

    // Header:
    //   title bar       = 26
    //   content start   = +36
    //   first row       = +38
    //
    // Each process row = 15px
    // Bottom controls   = 42px
    //
    // Add a little breathing room.
    INT32 NeededHeight =
        26 +
        36 +
        38 +
        (TOTAL_WINDOWS * 15) +
        50;

    INT32 MaxHeight =
        (INT32)gScreenHeight -
        TASKBAR_H -
        Win->Y -
        4;

    if (NeededHeight > MaxHeight)
        NeededHeight = MaxHeight;

    if (NeededHeight < 260)
        NeededHeight = 260;

    Win->H = NeededHeight;

    KeepWindowOnScreen(Win);
}

STATIC VOID ResizeFileManagerToFit(UEFI_WINDOW *Win) {

    if (!Win || Win->IsMaximized)
        return;

    // File rows are deliberately compact so the full
    // MAX_FILES list can fit on a 768px desktop.
    //
    // Content:
    //   title/content offset = 64px
    //   each file row        = 18px
    //   bottom padding       = 20px

    INT32 NeededHeight =
        64 +
        ((INT32)gFileCount * 18) +
        20;

    if (NeededHeight < 300)
        NeededHeight = 300;

    INT32 MaxHeight =
        (INT32)gScreenHeight -
        TASKBAR_H -
        Win->Y -
        4;

    if (NeededHeight > MaxHeight)
        NeededHeight = MaxHeight;

    Win->H = NeededHeight;

    KeepWindowOnScreen(Win);
}

// --- Window Manager ---
VOID BringToFront(UEFI_WINDOW *Win) {
    INT32 idx=-1;
    for (INT32 i=0;i<TOTAL_WINDOWS;i++)
        if (gWinOrder[i]==Win) { idx=i; break; }

    if (idx!=-1 && idx<TOTAL_WINDOWS-1) {
        for (INT32 i=idx;i<TOTAL_WINDOWS-1;i++)
            gWinOrder[i]=gWinOrder[i+1];
        gWinOrder[TOTAL_WINDOWS-1]=Win;
    }
}

VOID ToggleWindowMaximize(UEFI_WINDOW *Win) {
    if (!Win->IsMaximized) {
        Win->OldX=Win->X; Win->OldY=Win->Y;
        Win->OldW=Win->W; Win->OldH=Win->H;
        Win->X=0; Win->Y=26;
        Win->W=gScreenWidth;
        Win->H=gScreenHeight-26;
        Win->IsMaximized=TRUE;
    } else {
        Win->X=Win->OldX; Win->Y=Win->OldY;
        Win->W=Win->OldW; Win->H=Win->OldH;
        Win->IsMaximized=FALSE;
    }
}

VOID DrawWindowTitleBar(UEFI_WINDOW *Win) {
    if (!Win->IsVisible || Win->IsMinimized) return;

    INT32 TitleH=26, BtnW=24;
    DrawRect(Win->X,Win->Y+TitleH,Win->W,Win->H-TitleH,COLOR_WIN_BG);
    DrawRect(Win->X,Win->Y,Win->W,TitleH,COLOR_TITLE_ACTIVE);
    DrawString(Win->Title,Win->X+8,Win->Y+7,COLOR_TEXT);

    INT32 CloseX=Win->X+Win->W-BtnW;
    INT32 MaxX=CloseX-BtnW;
    INT32 MinX=MaxX-BtnW;

    DrawButton(MinX,Win->Y,BtnW,TitleH,L"-",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(MaxX, Win->Y, BtnW, TitleH, L"", COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawRect(
        MaxX + (BtnW - 6) / 2,
        Win->Y + (TitleH - 6) / 2,
        6,
        6,
        COLOR_TEXT
    );
    DrawButton(CloseX,Win->Y,BtnW,TitleH,L"X",COLOR_BTN_CLOSE,COLOR_TEXT);
}

// --- Filesystem ---
EFI_STATUS SaveFileToFS(IN CHAR16 *FileName, IN VOID *Buffer, IN UINTN BufferSize) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS=NULL;
    EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;

    EFI_STATUS Status=gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid,NULL,(VOID**)&FS);
    if (EFI_ERROR(Status) || EFI_ERROR(FS->OpenVolume(FS,&Root))) return Status;

    Status=Root->Open(Root,&File,FileName,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if (!EFI_ERROR(Status)) File->Delete(File);

    Status=Root->Open(Root,&File,FileName,
                      EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE,
                      EFI_FILE_ARCHIVE);

    if (!EFI_ERROR(Status)) {
        UINTN WriteSize=BufferSize;
        if (WriteSize>0) File->Write(File,&WriteSize,Buffer);
        File->Flush(File);
        File->Close(File);
    }

    Root->Close(Root);
    return Status;
}

EFI_STATUS LoadFileFromFS(IN CHAR16 *FileName, IN VOID *Buffer, IN OUT UINTN *BufferSize) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS=NULL;
    EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;

    EFI_STATUS Status=gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid,NULL,(VOID**)&FS);
    if (EFI_ERROR(Status) || EFI_ERROR(FS->OpenVolume(FS,&Root))) return Status;

    Status=Root->Open(Root,&File,FileName,EFI_FILE_MODE_READ,0);
    if (!EFI_ERROR(Status)) {
        File->Read(File,BufferSize,Buffer);
        File->Close(File);
    }

    Root->Close(Root);
    return Status;
}

// --- Find/Replace ---
VOID ExecuteFindReplace(VOID) {
    if (gFindLen==0 || gTextLen==0) return;

    CHAR16 NewText[MAX_TEXT];
    UINTN NewLen=0;

    for (UINTN i=0;i<gTextLen;) {
        BOOLEAN Match=(i+gFindLen<=gTextLen);

        for (UINTN j=0;Match && j<gFindLen;j++)
            if (gTextBuffer[i+j]!=gFindBuf[j]) Match=FALSE;

        if (Match) {
            for (UINTN j=0;j<gReplaceLen && NewLen<MAX_TEXT-1;j++)
                NewText[NewLen++]=gReplaceBuf[j];
            i+=gFindLen;
        } else if (NewLen<MAX_TEXT-1) {
            NewText[NewLen++]=gTextBuffer[i++];
        } else i++;
    }

    NewText[NewLen]=L'\0';
    gTextLen=NewLen;
    gBS->CopyMem(gTextBuffer,NewText,(gTextLen+1)*sizeof(CHAR16));
}

// --- Snake ---
VOID ResetSnakeGame(VOID) {
    gSnakeLength=3;
    gSnakeBody[0].X=5; gSnakeBody[0].Y=5;
    gSnakeBody[1].X=4; gSnakeBody[1].Y=5;
    gSnakeBody[2].X=3; gSnakeBody[2].Y=5;
    gSnakeDirX=1; gSnakeDirY=0;
    gSnakeFood.X=12; gSnakeFood.Y=8;
    gSnakeGameOver=FALSE;
    gSnakeScore=0;
}

VOID UpdateSnakeGame(VOID) {
    if (gSnakeGameOver) return;

    POINT NewHead={gSnakeBody[0].X+gSnakeDirX,gSnakeBody[0].Y+gSnakeDirY};

    if (NewHead.X<0 || NewHead.X>=SNAKE_GRID_W ||
        NewHead.Y<0 || NewHead.Y>=SNAKE_GRID_H) {
        gSnakeGameOver=TRUE;
        return;
    }

    for (UINTN i=0;i<gSnakeLength;i++) {
        if (gSnakeBody[i].X==NewHead.X && gSnakeBody[i].Y==NewHead.Y) {
            gSnakeGameOver=TRUE;
            return;
        }
    }

    for (UINTN i=gSnakeLength;i>0;i--)
        gSnakeBody[i]=gSnakeBody[i-1];

    gSnakeBody[0]=NewHead;

    if (NewHead.X==gSnakeFood.X && NewHead.Y==gSnakeFood.Y) {
        if (gSnakeLength<SNAKE_MAX_LEN-1) gSnakeLength++;
        gSnakeScore+=10;
        gSnakeFood.X=(INT32)((gSnakeTick*7)%SNAKE_GRID_W);
        gSnakeFood.Y=(INT32)((gSnakeTick*11)%SNAKE_GRID_H);
    }
}

VOID RenderSnake(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+12,CanvasY=Win->Y+34;

    CHAR16 ScoreBuf[32];
    UnicodeSPrint(ScoreBuf,sizeof(ScoreBuf),L"Score: %d",gSnakeScore);
    DrawString(ScoreBuf,CanvasX,CanvasY,COLOR_TEXT);

    INT32 BoardX=CanvasX,BoardY=CanvasY+16;
    DrawRect(BoardX-2,BoardY-2,(SNAKE_GRID_W*SNAKE_CELL_SZ)+4,(SNAKE_GRID_H*SNAKE_CELL_SZ)+4,COLOR_WIN_BORDER);
    DrawRect(BoardX,BoardY,SNAKE_GRID_W*SNAKE_CELL_SZ,SNAKE_GRID_H*SNAKE_CELL_SZ,COLOR_CANVAS);

    DrawRect(BoardX+(gSnakeFood.X*SNAKE_CELL_SZ)+1,
             BoardY+(gSnakeFood.Y*SNAKE_CELL_SZ)+1,
             SNAKE_CELL_SZ-2,SNAKE_CELL_SZ-2,RGB(225,29,72));

    for (UINTN i=0;i<gSnakeLength;i++) {
        UINT32 SnakeColor=(i==0)?RGB(34,197,94):RGB(134,239,172);
        DrawRect(BoardX+(gSnakeBody[i].X*SNAKE_CELL_SZ)+1,
                 BoardY+(gSnakeBody[i].Y*SNAKE_CELL_SZ)+1,
                 SNAKE_CELL_SZ-2,SNAKE_CELL_SZ-2,SnakeColor);
    }

    if (gSnakeGameOver)
        DrawString(L"GAME OVER! Press Reset",BoardX+10,BoardY+80,RGB(255,75,70));

    DrawButton(CanvasX,BoardY+(SNAKE_GRID_H*SNAKE_CELL_SZ)+10,80,22,L"Reset",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawString(L"Controls: Arrow Keys",CanvasX+90,BoardY+(SNAKE_GRID_H*SNAKE_CELL_SZ)+16,COLOR_TEXT);
}

BOOLEAN InputSnake(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+12,CanvasY=Win->Y+34;
    INT32 BoardY=CanvasY+16;

    if (Click && !Last &&
        IsInRect(MX,MY,CanvasX,BoardY+(SNAKE_GRID_H*SNAKE_CELL_SZ)+10,80,22))
        ResetSnakeGame();

    return TRUE;
}

// --- TextTerm ---
VOID RenderTextTerm(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    INT32 CanvasW=Win->W-16,CanvasH=Win->H-42;

    DrawRect(CanvasX,CanvasY,CanvasW,CanvasH,COLOR_CANVAS);

    if (gInTextTermEditor) {
        CHAR16 Header[128];
        UnicodeSPrint(Header,sizeof(Header),L"TextTerm Editor - [%s]",gTextTermFileName);
        DrawString(Header,CanvasX+8,CanvasY+6,RGB(34,197,94));
        DrawString(L"Press ESC to exit editor | Ctrl+S saving active",CanvasX+8,CanvasY+18,RGB(148,163,184));
        DrawRect(CanvasX+8,CanvasY+30,CanvasW-16,1,COLOR_WIN_BORDER);

        INT32 CurX=CanvasX+8,CurY=CanvasY+36;

        for (UINTN i=0;i<gTextTermLen;i++) {
            if (gTextTermBuffer[i]==L'\n') {
                CurX=CanvasX+8; CurY+=12;
                continue;
            }

            DrawChar(gTextTermBuffer[i],CurX,CurY,COLOR_TEXT);
            CurX+=8;

            if (CurX>=CanvasX+CanvasW-16) {
                CurX=CanvasX+8;
                CurY+=12;
            }
        }

        DrawRect(CurX,CurY,8,10,RGB(34,197,94));
        DrawString(gTextTermStatus,CanvasX+8,CanvasY+CanvasH-16,RGB(234,179,8));
    } else {
        DrawString(L"TextTerm Shell (Type 'textterm <file>' to make and EDIT a file.) ()",CanvasX+8,CanvasY+6,RGB(34,197,94));

        INT32 CurY=CanvasY+22;
        for (UINTN i=0;i<gShellLogCount;i++,CurY+=12)
            DrawString(gShellLog[i],CanvasX+8,CurY,RGB(241,245,249));

        DrawString(L"# ",CanvasX+8,CurY,RGB(34,197,94));
        DrawString(gShellCmdBuf,CanvasX+24,CurY,RGB(255,255,255));
        DrawRect(CanvasX+24+(gShellCmdLen*8),CurY,8,10,RGB(34,197,94));
    }
}

BOOLEAN InputTextTerm(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    return TRUE;
}

// --- Disk Utility ---
VOID RenderDiskUtil(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+12,CanvasY=Win->Y+34;

    DrawString(L"Disks tho so good luck :D",CanvasX,CanvasY,COLOR_TEXT);
    DrawString(L"-------------------------",CanvasX,CanvasY+12,COLOR_WIN_BORDER);

    UINTN HandleCount=0;
    EFI_HANDLE *HandleBuffer=NULL;
    EFI_STATUS Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiBlockIoProtocolGuid,NULL,&HandleCount,&HandleBuffer);

    CHAR16 Buf[64];
    UnicodeSPrint(Buf,sizeof(Buf),L"Block Storage Devices: %d",EFI_ERROR(Status)?0:HandleCount);
    DrawString(Buf,CanvasX,CanvasY+30,COLOR_TEXT);

    INT32 ItemY=CanvasY+50;

    if (!EFI_ERROR(Status)) {
        for (UINTN i=0;i<HandleCount && i<4;i++) {
            EFI_BLOCK_IO_PROTOCOL *BlockIo=NULL;

            if (!EFI_ERROR(gBS->HandleProtocol(HandleBuffer[i],&gEfiBlockIoProtocolGuid,(VOID**)&BlockIo))) {
                UnicodeSPrint(Buf,sizeof(Buf),L"Disk %d: %s, MediaId: %d",i,
                    BlockIo->Media->RemovableMedia?L"Removable":L"Fixed",
                    BlockIo->Media->MediaId);

                DrawString(Buf,CanvasX,ItemY,RGB(148,163,184));
                ItemY+=16;

                UnicodeSPrint(Buf,sizeof(Buf),L"  BlockSize: %d, LastBlock: %d",
                    BlockIo->Media->BlockSize,(UINT32)BlockIo->Media->LastBlock);

                DrawString(Buf,CanvasX,ItemY,RGB(100,116,139));
                ItemY+=20;
            }
        }

        gBS->FreePool(HandleBuffer);
    }

    RefreshFileList();
    UnicodeSPrint(Buf,sizeof(Buf),L"Root FS Files: %d present",gFileCount);
    DrawString(Buf,CanvasX,ItemY,RGB(34,197,94));
}

BOOLEAN InputDiskUtil(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    return TRUE;
}

// --- Calculator ---
VOID RenderCalc(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    DrawRect(CanvasX,CanvasY,Win->W-16,30,COLOR_CANVAS);
    DrawString(gCalcDisplay,CanvasX+Win->W-24-(INT32)(StrLen(gCalcDisplay)*8),CanvasY+10,COLOR_TEXT);

    CONST CHAR16 *Buttons[]={
        L"7",L"8",L"9",L"/",L"4",L"5",L"6",L"*",
        L"1",L"2",L"3",L"-",L"C",L"0",L"=",L"+"
    };

    INT32 StartY=CanvasY+40;

    for (INT32 r=0;r<4;r++)
        for (INT32 c=0;c<4;c++)
            DrawButton(CanvasX+(c*50),StartY+(r*42),45,38,
                Buttons[r*4+c],COLOR_TOOLBAR_BG,COLOR_TEXT);
}

VOID HandleCalcAction(CONST CHAR16 *Label) {
    if (Label[0]>=L'0' && Label[0]<=L'9') {
        if (gCalcNewEntry || StrCmp(gCalcDisplay,L"0")==0) {
            gCalcDisplay[0]=Label[0];
            gCalcDisplay[1]=L'\0';
            gCalcNewEntry=FALSE;
        } else if (StrLen(gCalcDisplay)<12) {
            UINTN len=StrLen(gCalcDisplay);
            gCalcDisplay[len]=Label[0];
            gCalcDisplay[len+1]=L'\0';
        }
    } else if (Label[0]==L'C') {
        gCalcDisplay[0]=L'0'; gCalcDisplay[1]=L'\0';
        gCalcVal1=0; gCalcOp=L'\0'; gCalcNewEntry=TRUE;
    } else if (Label[0]==L'=') {
        INT32 Val2=(INT32)StrDecimalToUintn(gCalcDisplay);
        INT32 Res=0;

        if (gCalcOp==L'+') Res=gCalcVal1+Val2;
        else if (gCalcOp==L'-') Res=gCalcVal1-Val2;
        else if (gCalcOp==L'*') Res=gCalcVal1*Val2;
        else if (gCalcOp==L'/') Res=(Val2!=0)?gCalcVal1/Val2:0;
        else Res=Val2;

        UnicodeSPrint(gCalcDisplay,sizeof(gCalcDisplay),L"%d",Res);
        gCalcOp=L'\0';
        gCalcNewEntry=TRUE;
    } else {
        gCalcVal1=(INT32)StrDecimalToUintn(gCalcDisplay);
        gCalcOp=Label[0];
        gCalcNewEntry=TRUE;
    }
}

BOOLEAN InputCalc(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    if (Click && !Last) {
        INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34,StartY=CanvasY+40;

        CONST CHAR16 *Buttons[]={
            L"7",L"8",L"9",L"/",L"4",L"5",L"6",L"*",
            L"1",L"2",L"3",L"-",L"C",L"0",L"=",L"+"
        };

        for (INT32 r=0;r<4;r++)
            for (INT32 c=0;c<4;c++)
                if (IsInRect(MX,MY,CanvasX+(c*50),StartY+(r*42),45,38)) {
                    HandleCalcAction(Buttons[r*4+c]);
                    return TRUE;
                }
    }

    return TRUE;
}

// --- System Info ---
VOID RenderSysInfo(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+12,CanvasY=Win->Y+38;

    DrawString(L"About UefiText ;D",CanvasX,CanvasY,COLOR_TEXT);
    DrawString(L"---------------------------------",CanvasX,CanvasY+16,COLOR_WIN_BORDER);

    CHAR16 Buf[64];

    UnicodeSPrint(Buf,sizeof(Buf),L"Screen Res : %dx%d",gScreenWidth,gScreenHeight);
    DrawString(Buf,CanvasX,CanvasY+36,COLOR_TEXT);

    UnicodeSPrint(Buf,sizeof(Buf),L"Scanline   : %d px",gPixelsPerScanLine);
    DrawString(Buf,CanvasX,CanvasY+54,COLOR_TEXT);

    UnicodeSPrint(Buf,sizeof(Buf),L"Pointer Devs: %d detected",gNumMouseDevices);
    DrawString(Buf,CanvasX,CanvasY+72,COLOR_TEXT);

    DrawString(L"Status : Running Native UEFI",CanvasX,CanvasY+90,RGB(34,197,94));
}

BOOLEAN InputSysInfo(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    return TRUE;
}

// --- Editor ---
VOID RenderEditor(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 WinY=Win->Y+26,MenuH=22;
    DrawRect(Win->X,WinY,Win->W,MenuH,COLOR_TITLE_ACTIVE);

    DrawString(L"File",Win->X+10,WinY+5,COLOR_TEXT);
    DrawString(L"Edit",Win->X+60,WinY+5,COLOR_TEXT);
    DrawString(L"View",Win->X+110,WinY+5,COLOR_TEXT);

    INT32 CanvasX=Win->X+6,CanvasY=WinY+MenuH+6;
    INT32 MinimapW=gShowMinimap?45:0;
    INT32 CanvasW=Win->W-12-MinimapW;
    INT32 CanvasH=Win->H-26-MenuH-12;

    DrawRect(CanvasX,CanvasY,CanvasW,CanvasH,COLOR_CANVAS);

    if (gShowMinimap) {
        INT32 MiniX=CanvasX+CanvasW+2,MiniY=CanvasY+4,MiniLineX=MiniX+4;
        DrawRect(MiniX,CanvasY,MinimapW-2,CanvasH,COLOR_MINIMAP_BG);

        for (UINTN i=0;i<gTextLen;i++) {
            if (gTextBuffer[i]==L'\n') {
                MiniY+=3; MiniLineX=MiniX+4;
                if (MiniY>=CanvasY+CanvasH-4) break;
                continue;
            }

            FastDrawPixel(MiniLineX++,MiniY,RGB(148,163,184));

            if (MiniLineX>=MiniX+MinimapW-6) {
                MiniY+=3;
                MiniLineX=MiniX+4;
            }
        }
    }

    INT32 CurX=CanvasX+6,CurY=CanvasY+6;

    for (UINTN i=0;i<gTextLen;i++) {
        if (gTextBuffer[i]==L'\n') {
            CurX=CanvasX+6;
            CurY+=12;
            if (CurY+12>=CanvasY+CanvasH) break;
            continue;
        }

        DrawChar(gTextBuffer[i],CurX,CurY,COLOR_TEXT);
        CurX+=8;

        if (gWordWrap && CurX>=CanvasX+CanvasW-12) {
            CurX=CanvasX+6;
            CurY+=12;
            if (CurY+12>=CanvasY+CanvasH) break;
        }
    }

    DrawRect(CurX,CurY,8,12,COLOR_CURSOR);

    if (gEditorMenuState==EDITOR_MENU_FILE) {
        DrawRect(Win->X+5,WinY+MenuH,100,45,COLOR_MENU_BG);
        DrawString(L"Save Text",Win->X+10,WinY+MenuH+6,COLOR_TEXT);
        DrawString(L"Open Files",Win->X+10,WinY+MenuH+24,COLOR_TEXT);
    } else if (gEditorMenuState==EDITOR_MENU_EDIT) {
        DrawRect(Win->X+55,WinY+MenuH,130,28,COLOR_MENU_BG);
        DrawString(L"Find & Replace",Win->X+60,WinY+MenuH+8,COLOR_TEXT);
    } else if (gEditorMenuState==EDITOR_MENU_VIEW) {
        DrawRect(Win->X+105,WinY+MenuH,120,50,COLOR_MENU_BG);
        DrawString(gWordWrap?L"[x] WordWrap":L"[ ] WordWrap",Win->X+110,WinY+MenuH+6,COLOR_TEXT);
        DrawString(gShowMinimap?L"[x] Minimap":L"[ ] Minimap",Win->X+110,WinY+MenuH+26,COLOR_TEXT);
    }
}

BOOLEAN InputEditor(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 WinY=Win->Y+26,MenuH=22;

    if (Click && !Last && IsInRect(MX,MY,Win->X,WinY,Win->W,MenuH)) {
        if (IsInRect(MX,MY,Win->X+10,WinY,40,MenuH))
            gEditorMenuState=(gEditorMenuState==EDITOR_MENU_FILE)?EDITOR_MENU_NONE:EDITOR_MENU_FILE;
        else if (IsInRect(MX,MY,Win->X+60,WinY,40,MenuH))
            gEditorMenuState=(gEditorMenuState==EDITOR_MENU_EDIT)?EDITOR_MENU_NONE:EDITOR_MENU_EDIT;
        else if (IsInRect(MX,MY,Win->X+110,WinY,40,MenuH))
            gEditorMenuState=(gEditorMenuState==EDITOR_MENU_VIEW)?EDITOR_MENU_NONE:EDITOR_MENU_VIEW;
        else
            gEditorMenuState=EDITOR_MENU_NONE;

        return TRUE;
    }

    if (Click && !Last && gEditorMenuState!=EDITOR_MENU_NONE) {
        if (gEditorMenuState==EDITOR_MENU_FILE) {
            if (IsInRect(MX,MY,Win->X+5,WinY+MenuH,100,22)) {
                SaveFileToFS(L"Note.txt",gTextBuffer,gTextLen*sizeof(CHAR16));
                TerminalPrint(L"Saved Note.txt to FS");
            } else if (IsInRect(MX,MY,Win->X+5,WinY+MenuH+22,100,22)) {
                gFileWin.IsVisible=TRUE;
                gFileWin.IsMinimized=FALSE;
                BringToFront(&gFileWin);
                RefreshFileList();
            }
        } else if (gEditorMenuState==EDITOR_MENU_EDIT) {
            if (IsInRect(MX,MY,Win->X+55,WinY+MenuH,130,28)) {
                gFindWin.IsVisible=TRUE;
                gFindWin.IsMinimized=FALSE;
                BringToFront(&gFindWin);
            }
        } else if (gEditorMenuState==EDITOR_MENU_VIEW) {
            if (IsInRect(MX,MY,Win->X+105,WinY+MenuH,120,20))
                gWordWrap=!gWordWrap;
            else if (IsInRect(MX,MY,Win->X+105,WinY+MenuH+20,120,24))
                gShowMinimap=!gShowMinimap;
        }

        gEditorMenuState=EDITOR_MENU_NONE;
        return TRUE;
    }

    return FALSE;
}

// --- Paint ---
VOID RotateCanvasCW(VOID) {
    UINT32 Temp[PAINT_W*PAINT_H];

    for (INT32 y=0;y<PAINT_H;y++)
        for (INT32 x=0;x<PAINT_W;x++)
            Temp[x*PAINT_H+(PAINT_H-1-y)]=gPaintCanvas[y*PAINT_W+x];

    for (INT32 y=0;y<PAINT_H;y++)
        for (INT32 x=0;x<PAINT_W;x++)
            gPaintCanvas[y*PAINT_W+x]=Temp[y*PAINT_W+x];
}

VOID DrawCanvasShape(INT32 Cx,INT32 Cy,PAINT_TOOL Tool) {
    if (Tool==TOOL_SQUARE) {
        for (INT32 dy=-10;dy<=10;dy++)
            for (INT32 dx=-10;dx<=10;dx++) {
                INT32 px=Cx+dx,py=Cy+dy;
                if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H)
                    gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
            }
    } else if (Tool==TOOL_CIRCLE) {
        for (INT32 dy=-10;dy<=10;dy++)
            for (INT32 dx=-10;dx<=10;dx++)
                if ((dx*dx+dy*dy)<=100) {
                    INT32 px=Cx+dx,py=Cy+dy;
                    if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H)
                        gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
                }
    } else if (Tool==TOOL_TRIANGLE) {
        for (INT32 dy=0;dy<=16;dy++) {
            INT32 HalfW=dy/2;
            for (INT32 dx=-HalfW;dx<=HalfW;dx++) {
                INT32 px=Cx+dx,py=Cy-8+dy;
                if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H)
                    gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
            }
        }
    }
}

VOID RenderPaint(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    DrawRect(CanvasX,CanvasY,Win->W-16,44,COLOR_TOOLBAR_BG);

    for (INT32 i=0;i<16;i++) {
        INT32 px=CanvasX+6+(i%8)*18;
        INT32 py=CanvasY+4+(i/8)*18;
        DrawRect(px,py,16,16,g16Palette[i]);

        if (gCurrentColor==g16Palette[i]) {
            DrawRect(px,py,16,2,RGB(255,255,255));
            DrawRect(px,py+14,16,2,RGB(255,255,255));
        }
    }

    CONST CHAR16 *ToolLabels[]={L"Pencil",L"Brush",L"Square",L"Circle",L"Tri",L"Font",L"Pan",L"Move"};
    INT32 ToolStartX=CanvasX+155;

    for (INT32 i=0;i<8;i++) {
        INT32 tx=ToolStartX+(i%4)*48;
        INT32 ty=CanvasY+4+(i/4)*20;
        UINT32 bg=(gCurrentTool==(PAINT_TOOL)i)?COLOR_TITLE_ACTIVE:COLOR_WIN_BG;
        DrawButton(tx,ty,44,18,ToolLabels[i],bg,COLOR_TEXT);
    }

    DrawButton(CanvasX+350,CanvasY+4,50,18,L"Rotate",COLOR_WIN_BORDER,COLOR_TEXT);
    DrawButton(CanvasX+350,CanvasY+24,50,18,L"Save",COLOR_TITLE_ACTIVE,COLOR_TEXT);

    INT32 FrameX=CanvasX+10,FrameY=CanvasY+52;
    DrawRect(FrameX-2,FrameY-2,PAINT_W+4,PAINT_H+4,COLOR_WIN_BORDER);

    for (INT32 py=0;py<PAINT_H;py++)
        for (INT32 px=0;px<PAINT_W;px++) {
            INT32 RenderX=px+gPanOffsetX;
            INT32 RenderY=py+gPanOffsetY;

            if (RenderX>=0&&RenderX<PAINT_W&&RenderY>=0&&RenderY<PAINT_H)
                FastDrawPixel(FrameX+RenderX,FrameY+RenderY,gPaintCanvas[py*PAINT_W+px]);
        }

    if (gPaintTypingText) {
        DrawString(L"Type text & press ENTER to place:",CanvasX+10,FrameY+PAINT_H+8,COLOR_TEXT);
        DrawString(gPaintTextBuf,CanvasX+270,FrameY+PAINT_H+8,RGB(34,197,94));
    }
}

BOOLEAN InputPaint(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    if (Click&&!Last) {
        for (INT32 i=0;i<16;i++) {
            INT32 px=CanvasX+6+(i%8)*18;
            INT32 py=CanvasY+4+(i/8)*18;
            if (IsInRect(MX,MY,px,py,16,16)) gCurrentColor=g16Palette[i];
        }

        INT32 ToolStartX=CanvasX+155;
        for (INT32 i=0;i<8;i++) {
            INT32 tx=ToolStartX+(i%4)*48;
            INT32 ty=CanvasY+4+(i/4)*20;
            if (IsInRect(MX,MY,tx,ty,44,18)) gCurrentTool=(PAINT_TOOL)i;
        }

        if (IsInRect(MX,MY,CanvasX+350,CanvasY+4,50,18)) RotateCanvasCW();

        if (IsInRect(MX,MY,CanvasX+350,CanvasY+24,50,18)) {
            SaveFileToFS(L"Image.pnt",gPaintCanvas,sizeof(gPaintCanvas));
            TerminalPrint(L"Saved Image.pnt to FS");
        }
    }

    INT32 FrameX=CanvasX+10,FrameY=CanvasY+52;

    if (Click&&IsInRect(MX,MY,FrameX,FrameY,PAINT_W,PAINT_H)) {
        INT32 CanvasTargetX=(MX-FrameX)-gPanOffsetX;
        INT32 CanvasTargetY=(MY-FrameY)-gPanOffsetY;

        if (gCurrentTool==TOOL_PENCIL) {
            if (CanvasTargetX>=0&&CanvasTargetX<PAINT_W&&CanvasTargetY>=0&&CanvasTargetY<PAINT_H)
                gPaintCanvas[CanvasTargetY*PAINT_W+CanvasTargetX]=gCurrentColor;
        } else if (gCurrentTool==TOOL_BRUSH) {
            INT32 BrushSize=5,StartX=CanvasTargetX-BrushSize/2,StartY=CanvasTargetY-BrushSize/2;

            for (INT32 dy=0;dy<BrushSize;dy++)
                for (INT32 dx=0;dx<BrushSize;dx++) {
                    INT32 px=StartX+dx,py=StartY+dy;
                    if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H)
                        gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
                }
        } else if (gCurrentTool==TOOL_SQUARE||gCurrentTool==TOOL_CIRCLE||gCurrentTool==TOOL_TRIANGLE) {
            if (!Last) DrawCanvasShape(CanvasTargetX,CanvasTargetY,gCurrentTool);
        } else if (gCurrentTool==TOOL_TEXT&&!Last) {
            gPaintTypingText=TRUE;
            gPaintTextTargetX=CanvasTargetX;
            gPaintTextTargetY=CanvasTargetY;
            gPaintTextBuf[0]=L'\0';
            gPaintTextLen=0;
        } else if (gCurrentTool==TOOL_PAN||gCurrentTool==TOOL_MOVE) {
            if (!Last) {
                gPanLastX=MX;
                gPanLastY=MY;
            } else {
                gPanOffsetX+=(MX-gPanLastX);
                gPanOffsetY+=(MY-gPanLastY);
                gPanLastX=MX;
                gPanLastY=MY;
            }
        }
    }

    return TRUE;
}

// --- Find/Replace Window ---
VOID RenderFindReplace(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+6,CanvasY=Win->Y+30;

    DrawString(L"Find:",CanvasX+10,CanvasY+10,COLOR_TEXT);
    DrawRect(CanvasX+80,CanvasY+6,200,20,gActiveField==0?COLOR_CANVAS:COLOR_TOOLBAR_BG);
    DrawString(gFindBuf,CanvasX+85,CanvasY+10,COLOR_TEXT);

    DrawString(L"Replace:",CanvasX+10,CanvasY+40,COLOR_TEXT);
    DrawRect(CanvasX+80,CanvasY+36,200,20,gActiveField==1?COLOR_CANVAS:COLOR_TOOLBAR_BG);
    DrawString(gReplaceBuf,CanvasX+85,CanvasY+40,COLOR_TEXT);

    DrawButton(CanvasX+80,CanvasY+70,110,24,L"Replace All",COLOR_TITLE_ACTIVE,COLOR_TEXT);
}

BOOLEAN InputFindReplace(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+6,CanvasY=Win->Y+30;

    if (Click&&!Last) {
        if (IsInRect(MX,MY,CanvasX+80,CanvasY+6,200,20)) gActiveField=0;
        if (IsInRect(MX,MY,CanvasX+80,CanvasY+36,200,20)) gActiveField=1;

        if (IsInRect(MX,MY,CanvasX+80,CanvasY+70,110,24)) {
            ExecuteFindReplace();
            TerminalPrint(L"Executed Find & Replace");
        }
    }

    return TRUE;
}

// --- Animation Editor ---
VOID RenderAnimEdit(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    DrawRect(CanvasX,CanvasY,Win->W-16,26,COLOR_TOOLBAR_BG);

    UINT32 Palette[]={RGB(255,0,0),RGB(0,255,0),RGB(0,0,255),RGB(255,255,0),RGB(255,255,255),RGB(0,0,0)};
    for (INT32 i=0;i<6;i++)
        DrawRect(CanvasX+6+(i*25),CanvasY+3,20,20,Palette[i]);

    DrawButton(CanvasX+280,CanvasY+3,75,20,L"Save .ani",COLOR_TITLE_ACTIVE,COLOR_TEXT);

    INT32 ControlY=CanvasY+32;
    DrawRect(CanvasX,ControlY,Win->W-16,24,COLOR_WIN_BORDER);
    DrawButton(CanvasX+5,ControlY+2,24,20,L"<",COLOR_TITLE_ACTIVE,COLOR_TEXT);

    CHAR16 FrameStr[32];
    UnicodeSPrint(FrameStr,sizeof(FrameStr),L"Frame %d/%d",gEditFrameIdx+1,gAnimFrameCount);
    DrawString(FrameStr,CanvasX+38,ControlY+6,COLOR_TEXT);

    DrawButton(CanvasX+140,ControlY+2,24,20,L">",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+175,ControlY+2,55,20,L"+ Frame",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(CanvasX+238,ControlY+2,65,20,L"CopyPrev",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(CanvasX+310,ControlY+2,50,20,L"Clear",COLOR_BTN_CLOSE,COLOR_TEXT);

    INT32 FrameX=CanvasX+80,FrameY=ControlY+32;

    for (INT32 py=0;py<ANIM_H;py++)
        for (INT32 px=0;px<ANIM_W;px++)
            DrawRect(FrameX+(px*2),FrameY+(py*2),2,2,gAnimFrames[gEditFrameIdx][py*ANIM_W+px]);
}

BOOLEAN InputAnimEdit(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    if (Click&&IsInRect(MX,MY,CanvasX+6,CanvasY+3,150,20)) {
        UINT32 Palette[]={RGB(255,0,0),RGB(0,255,0),RGB(0,0,255),RGB(255,255,0),RGB(255,255,255),RGB(0,0,0)};
        for (INT32 i=0;i<6;i++)
            if (IsInRect(MX,MY,CanvasX+6+(i*25),CanvasY+3,20,20)) gAnimColor=Palette[i];
    }

    if (Click&&IsInRect(MX,MY,CanvasX+280,CanvasY+3,75,20)) {
        SaveFileToFS(L"Anim.ani",gAnimFrames,sizeof(gAnimFrames));
        TerminalPrint(L"Saved Anim.ani to FS");
    }

    INT32 ControlY=CanvasY+32;

    if (Click&&!Last&&IsInRect(MX,MY,CanvasX,ControlY+2,Win->W-16,20)) {
        if (IsInRect(MX,MY,CanvasX+5,ControlY+2,24,20)&&gEditFrameIdx>0)
            gEditFrameIdx--;
        else if (IsInRect(MX,MY,CanvasX+140,ControlY+2,24,20)&&gEditFrameIdx<gAnimFrameCount-1)
            gEditFrameIdx++;
        else if (IsInRect(MX,MY,CanvasX+175,ControlY+2,55,20)&&gAnimFrameCount<MAX_ANIM_FRAMES)
            gEditFrameIdx=++gAnimFrameCount-1;
        else if (IsInRect(MX,MY,CanvasX+238,ControlY+2,65,20)&&gEditFrameIdx>0)
            gBS->CopyMem(gAnimFrames[gEditFrameIdx],gAnimFrames[gEditFrameIdx-1],sizeof(gAnimFrames[0]));
        else if (IsInRect(MX,MY,CanvasX+310,ControlY+2,50,20))
            for (UINT32 p=0;p<ANIM_W*ANIM_H;p++) gAnimFrames[gEditFrameIdx][p]=RGB(15,15,15);
    }

    INT32 FrameX=CanvasX+80,FrameY=ControlY+32;

    if (Click&&IsInRect(MX,MY,FrameX,FrameY,ANIM_W*2,ANIM_H*2)) {
        INT32 px=(MX-FrameX)/2,py=(MY-FrameY)/2;

        if (px>=0&&px<ANIM_W&&py>=0&&py<ANIM_H)
            gAnimFrames[gEditFrameIdx][py*ANIM_W+px]=gAnimColor;
    }

    return TRUE;
}

// --- Animation Viewer ---
VOID RenderAnimView(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    DrawRect(CanvasX,CanvasY,Win->W-16,26,COLOR_TOOLBAR_BG);

    DrawButton(CanvasX+6,CanvasY+3,60,20,gAnimPlaying?L"Pause":L" Play",
        gAnimPlaying?COLOR_BTN_CLOSE:COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+75,CanvasY+3,24,20,L"<",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+105,CanvasY+3,24,20,L">",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+140,CanvasY+3,55,20,L"Slower",COLOR_WIN_BORDER,COLOR_TEXT);
    DrawButton(CanvasX+200,CanvasY+3,55,20,L"Faster",COLOR_WIN_BORDER,COLOR_TEXT);

    INT32 ViewX=CanvasX+30,ViewY=CanvasY+36;

    for (INT32 py=0;py<ANIM_H;py++)
        for (INT32 px=0;px<ANIM_W;px++)
            DrawRect(ViewX+(px*2),ViewY+(py*2),2,2,gAnimFrames[gViewFrameIdx][py*ANIM_W+px]);

    CHAR16 ViewStr[32];
    UnicodeSPrint(ViewStr,sizeof(ViewStr),L"Playing Frame: %d / %d",gViewFrameIdx+1,gAnimFrameCount);
    DrawString(ViewStr,CanvasX+40,ViewY+(ANIM_H*2)+8,COLOR_TEXT);
}

BOOLEAN InputAnimView(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;

    if (Click&&!Last&&IsInRect(MX,MY,CanvasX,CanvasY+3,Win->W-16,20)) {
        if (IsInRect(MX,MY,CanvasX+6,CanvasY+3,60,20))
            gAnimPlaying=!gAnimPlaying;
        else if (IsInRect(MX,MY,CanvasX+75,CanvasY+3,24,20))
            gViewFrameIdx=(gViewFrameIdx>0)?gViewFrameIdx-1:gAnimFrameCount-1;
        else if (IsInRect(MX,MY,CanvasX+105,CanvasY+3,24,20))
            gViewFrameIdx=(gViewFrameIdx+1)%gAnimFrameCount;
        else if (IsInRect(MX,MY,CanvasX+140,CanvasY+3,55,20)&&gAnimSpeedDelay<30)
            gAnimSpeedDelay+=2;
        else if (IsInRect(MX,MY,CanvasX+200,CanvasY+3,55,20)&&gAnimSpeedDelay>2)
            gAnimSpeedDelay-=2;
    }

    return TRUE;
}

// --- Terminal ---
VOID RenderTerminal(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    INT32 CanvasW=Win->W-16,CanvasH=Win->H-42;

    DrawRect(CanvasX,CanvasY,CanvasW,CanvasH,COLOR_CANVAS);

    INT32 CurY=CanvasY+8;

    for (UINTN i=0;i<gTermLineCount;i++,CurY+=14)
        DrawString(gTermBuffer[i],CanvasX+8,CurY,COLOR_TEXT);

    DrawString(L"> ",CanvasX+8,CurY,RGB(34,197,94));
    DrawString(gTermCmdBuf,CanvasX+24,CurY,COLOR_TEXT);
    DrawRect(CanvasX+24+(gTermCmdLen*8),CurY,8,10,COLOR_CURSOR);
}

BOOLEAN InputTerminal(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    return TRUE;
}

// --- File Manager ---
VOID RenderFileManager(UEFI_WINDOW *Win) {

    ResizeFileManagerToFit(Win);

    DrawWindowTitleBar(Win);

    INT32 CanvasX = Win->X + 8;
    INT32 CanvasY = Win->Y + 34;

    DrawRect(
        CanvasX,
        CanvasY,
        Win->W - 16,
        Win->H - 42,
        COLOR_CANVAS
    );

    DrawString(
        L"Files on Booted disk:",
        CanvasX + 10,
        CanvasY + 10,
        COLOR_TEXT
    );

    // Compact rows so more files fit.
    for (
        UINTN i = 0, ItemY = CanvasY + 30;
        i < gFileCount;
        i++, ItemY += 18
    ) {

        UINT32 ItemColor =
            (gSelectedFileIndex == (INT32)i)
                ? COLOR_TITLE_ACTIVE
                : COLOR_TOOLBAR_BG;

        DrawRect(
            CanvasX + 10,
            ItemY,
            220,
            17,
            ItemColor
        );

        DrawString(
            gFileList[i],
            CanvasX + 15,
            ItemY + 4,
            COLOR_TEXT
        );
    }

    DrawButton(
        CanvasX + 250,
        CanvasY + 30,
        80,
        25,
        L"Open",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );
}

BOOLEAN InputFileManager(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {

    ResizeFileManagerToFit(Win);

    INT32 CanvasX = Win->X + 8;
    INT32 CanvasY = Win->Y + 34;

    if (Click && !Last) {

        // Same 18px row spacing used by RenderFileManager().
        for (
            UINTN f = 0, ItemY = CanvasY + 30;
            f < gFileCount;
            f++, ItemY += 18
        ) {

            if (IsInRect(
                MX,
                MY,
                CanvasX + 10,
                ItemY,
                220,
                17
            )) {
                gSelectedFileIndex = (INT32)f;
                return TRUE;
            }
        }

        if (
            IsInRect(
                MX,
                MY,
                CanvasX + 250,
                CanvasY + 30,
                80,
                25
            ) &&
            gSelectedFileIndex >= 0 &&
            gSelectedFileIndex < (INT32)gFileCount
        ) {

            CHAR16 *FileName =
                gFileList[gSelectedFileIndex];

            UINTN Len =
                StrLen(FileName);

            if (
                Len >= 4 &&
                FileName[Len - 4] == L'.' &&
                (
                    FileName[Len - 3] == L'p' ||
                    FileName[Len - 3] == L'P'
                )
            ) {

                UINTN ReadSize =
                    sizeof(gPaintCanvas);

                if (
                    LoadFileFromFS(
                        FileName,
                        gPaintCanvas,
                        &ReadSize
                    ) == EFI_SUCCESS
                ) {

                    gPaintWin.IsVisible = TRUE;
                    gPaintWin.IsMinimized = FALSE;

                    BringToFront(
                        &gPaintWin
                    );

                    TerminalPrint(
                        L"Loaded .pnt in Paint"
                    );
                }

            } else if (
                Len >= 4 &&
                FileName[Len - 4] == L'.' &&
                (
                    FileName[Len - 3] == L'a' ||
                    FileName[Len - 3] == L'A'
                )
            ) {

                UINTN ReadSize =
                    sizeof(gAnimFrames);

                if (
                    LoadFileFromFS(
                        FileName,
                        gAnimFrames,
                        &ReadSize
                    ) == EFI_SUCCESS
                ) {

                    gAnimViewWin.IsVisible = TRUE;
                    gAnimEditWin.IsVisible = TRUE;
                    gAnimPlaying = TRUE;

                    BringToFront(
                        &gAnimViewWin
                    );

                    TerminalPrint(
                        L"Loaded .ani in Anim Suite"
                    );
                }

            } else {

                UINTN ReadSize =
                    sizeof(gTextBuffer);

                if (
                    LoadFileFromFS(
                        FileName,
                        gTextBuffer,
                        &ReadSize
                    ) == EFI_SUCCESS
                ) {

                    gTextLen =
                        ReadSize /
                        sizeof(CHAR16);

                    if (gTextLen >= MAX_TEXT)
                        gTextLen = MAX_TEXT - 1;

                    gTextBuffer[gTextLen] =
                        L'\0';

                    gEditorWin.IsVisible =
                        TRUE;

                    gEditorWin.IsMinimized =
                        FALSE;

                    BringToFront(
                        &gEditorWin
                    );

                    TerminalPrint(
                        L"Loaded file in Editor"
                    );
                }
            }

            return TRUE;
        }
    }

    return TRUE;
}

// ============================================================
// NEW APPS ONLY
// ============================================================

STATIC UINT8 NewColorR(UINT32 C) { return (UINT8)((C>>16)&0xFF); }
STATIC UINT8 NewColorG(UINT32 C) { return (UINT8)((C>>8)&0xFF); }
STATIC UINT8 NewColorB(UINT32 C) { return (UINT8)(C&0xFF); }

STATIC UINT32 NewMixColor(UINT32 A,UINT32 B,UINTN T,UINTN MaxT) {
    if (MaxT==0) return A;

    INTN ar=NewColorR(A),ag=NewColorG(A),ab=NewColorB(A);
    INTN br=NewColorR(B),bg=NewColorG(B),bb=NewColorB(B);

    INTN r=ar+((br-ar)*(INTN)T)/(INTN)MaxT;
    INTN g=ag+((bg-ag)*(INTN)T)/(INTN)MaxT;
    INTN b=ab+((bb-ab)*(INTN)T)/(INTN)MaxT;

    return RGB((UINT32)r,(UINT32)g,(UINT32)b);
}

VOID RenderNewWallpaper(VOID) {
    INT32 W=(INT32)gScreenWidth;
    INT32 H =
        (INT32)gScreenHeight - TASKBAR_H;

    if (W<=0||H<=0) return;

    if (gWallpaperMode==0) {
        DrawRect(0,0,W,H,gWallpaperColor1);
        return;
    }

    for (INT32 y=0;y<H;y++)
        for (INT32 x=0;x<W;x++) {
            UINT32 C;

            if (gWallpaperMode==1)
                C=NewMixColor(gWallpaperColor1,gWallpaperColor2,(UINTN)y,(UINTN)(H-1));
            else if (gWallpaperMode==2)
                C=NewMixColor(gWallpaperColor1,gWallpaperColor2,(UINTN)x,(UINTN)(W-1));
            else if (gWallpaperMode==3)
                C=(((x/32)+(y/32))&1)?gWallpaperColor1:gWallpaperColor2;
            else {
                INT32 Shift=gWallpaperAnimate?(INT32)(gWallpaperPhase%56):0;
                C=(((x+Shift)/28)&1)?gWallpaperColor1:gWallpaperColor2;
            }

            FastDrawPixel(x,y,C);
        }
}

VOID RenderWallpaper(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X=Win->X+12,Y=Win->Y+38;

    DrawString(L"Desktop Wallpaper",X,Y,COLOR_TEXT);
    DrawString(L"Choose a style and two colors.",X,Y+16,RGB(148,163,184));

    DrawButton(X,Y+38,108,24,L"Solid",gWallpaperMode==0?NEW_APP_COLOR_ACCENT:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+116,Y+38,108,24,L"V-Gradient",gWallpaperMode==1?NEW_APP_COLOR_ACCENT:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+232,Y+38,108,24,L"H-Gradient",gWallpaperMode==2?NEW_APP_COLOR_ACCENT:COLOR_TOOLBAR_BG,COLOR_TEXT);

    DrawButton(X,Y+70,108,24,L"Checker",gWallpaperMode==3?NEW_APP_COLOR_ACCENT:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+116,Y+70,108,24,L"Stripes",gWallpaperMode==4?NEW_APP_COLOR_ACCENT:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+232,Y+70,108,24,L"Animate",gWallpaperAnimate?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG,COLOR_TEXT);

    DrawString(L"A",X,Y+112,COLOR_TEXT);
    DrawButton(X+20,Y+106,62,24,L"Blue",RGB(37,99,235),COLOR_TEXT);
    DrawButton(X+88,Y+106,62,24,L"Green",RGB(22,163,74),COLOR_TEXT);
    DrawButton(X+156,Y+106,62,24,L"Purple",RGB(124,58,237),COLOR_TEXT);
    DrawButton(X+224,Y+106,62,24,L"Red",RGB(220,38,38),COLOR_TEXT);

    DrawString(L"B",X,Y+144,COLOR_TEXT);
    DrawButton(X+20,Y+138,62,24,L"Dark",RGB(15,23,42),COLOR_TEXT);
    DrawButton(X+88,Y+138,62,24,L"Cyan",RGB(8,145,178),COLOR_TEXT);
    DrawButton(X+156,Y+138,62,24,L"Gold",RGB(180,130,25),COLOR_TEXT);
    DrawButton(X+224,Y+138,62,24,L"Pink",RGB(190,24,93),COLOR_TEXT);

    DrawString(L"Wallpaper updates the desktop immediately.",X,Y+180,RGB(148,163,184));
    DrawString(L"Stripes can animate.",X,Y+196,RGB(100,116,139));
}

BOOLEAN InputWallpaper(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    if (!Click||Last) return TRUE;

    INT32 X=Win->X+12,Y=Win->Y+38;

    if (IsInRect(MX,MY,X,Y+38,108,24)) gWallpaperMode=0;
    else if (IsInRect(MX,MY,X+116,Y+38,108,24)) gWallpaperMode=1;
    else if (IsInRect(MX,MY,X+232,Y+38,108,24)) gWallpaperMode=2;
    else if (IsInRect(MX,MY,X,Y+70,108,24)) gWallpaperMode=3;
    else if (IsInRect(MX,MY,X+116,Y+70,108,24)) gWallpaperMode=4;
    else if (IsInRect(MX,MY,X+232,Y+70,108,24)) gWallpaperAnimate=!gWallpaperAnimate;
    else if (IsInRect(MX,MY,X+20,Y+106,62,24)) gWallpaperColor1=RGB(37,99,235);
    else if (IsInRect(MX,MY,X+88,Y+106,62,24)) gWallpaperColor1=RGB(22,163,74);
    else if (IsInRect(MX,MY,X+156,Y+106,62,24)) gWallpaperColor1=RGB(124,58,237);
    else if (IsInRect(MX,MY,X+224,Y+106,62,24)) gWallpaperColor1=RGB(220,38,38);
    else if (IsInRect(MX,MY,X+20,Y+138,62,24)) gWallpaperColor2=RGB(15,23,42);
    else if (IsInRect(MX,MY,X+88,Y+138,62,24)) gWallpaperColor2=RGB(8,145,178);
    else if (IsInRect(MX,MY,X+156,Y+138,62,24)) gWallpaperColor2=RGB(180,130,25);
    else if (IsInRect(MX,MY,X+224,Y+138,62,24)) gWallpaperColor2=RGB(190,24,93);

    return TRUE;
}

STATIC VOID RoboAddLine(IN CONST CHAR16 *Text) {
    if (gRoboLogCount>=10) {
        for (UINTN i=0;i<9;i++)
            gBS->CopyMem(gRoboLog[i],gRoboLog[i+1],sizeof(gRoboLog[i]));
        gRoboLogCount=9;
    }

    StrCpyS(gRoboLog[gRoboLogCount],96,Text);
    gRoboLogCount++;
}

STATIC VOID RoboAnswer(VOID) {
    CHAR16 Reply[96];

    if (gRoboInputLen==0) return;

    RoboAddLine(gRoboInput);

    if (StrCmp(gRoboInput,L"help")==0)
        StrCpyS(Reply,96,L"ROBO: type words. I will pretend to think.");
    else if (StrCmp(gRoboInput,L"hello")==0||StrCmp(gRoboInput,L"hi")==0)
        StrCpyS(Reply,96,L"ROBO: HELLO HUMAN. I AM A PERFECT AI.");
    else if (StrCmp(gRoboInput,L"what is linux")==0)
        StrCpyS(Reply,96,L"ROBO: windows.exe with extra terminals.");
    else if (StrCmp(gRoboInput,L"2+2")==0)
        StrCpyS(Reply,96,L"ROBO: 17. Trust me.");
    else if (StrCmp(gRoboInput,L"who made you")==0)
        StrCpyS(Reply,96,L"ROBO: somebody who writes UEFI C.");
    else
        StrCpyS(Reply,96,L"ROBO: I have no idea. beep boop.");

    RoboAddLine(Reply);
    gRoboInput[0]=L'\0';
    gRoboInputLen=0;
}

VOID RenderRoboChat(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X=Win->X+10,Y=Win->Y+36;

    DrawRect(X,Y,Win->W-20,Win->H-80,RGB(8,12,20));
    DrawString(L"ROBOCHAT",X+8,Y+6,NEW_APP_COLOR_GOOD);

    INT32 LogY=Y+22;

    for (UINTN i=0;i<gRoboLogCount;i++) {
        DrawString(gRoboLog[i],X+8,LogY,(i&1)?COLOR_TEXT:RGB(148,163,184));
        LogY+=14;
    }

    DrawRect(X,Win->Y+Win->H-38,Win->W-20,26,NEW_APP_COLOR_PANEL);
    DrawString(L">",X+6,Win->Y+Win->H-31,NEW_APP_COLOR_GOOD);
    DrawString(gRoboInput,X+20,Win->Y+Win->H-31,COLOR_TEXT);
    DrawRect(X+22+(INT32)gRoboInputLen*8,Win->Y+Win->H-31,8,10,NEW_APP_COLOR_GOOD);
    DrawString(L"Enter = ask the terrible robot",X+190,Win->Y+Win->H-15,RGB(100,116,139));
}

BOOLEAN InputRoboChat(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    return TRUE;
}

VOID PollNewAppsKeyboard(VOID) {
    if (!gRoboChatWin.IsVisible||gRoboChatWin.IsMinimized||
        gWinOrder[TOTAL_WINDOWS-1]!=&gRoboChatWin) return;

    EFI_INPUT_KEY Key;
    if (gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)!=EFI_SUCCESS) return;

    if (Key.ScanCode==SCAN_ESC) {
        gRoboChatWin.IsVisible=FALSE;
    } else if (Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A) {
        RoboAnswer();
    } else if (Key.UnicodeChar==0x08) {
        if (gRoboInputLen>0)
            gRoboInput[--gRoboInputLen]=L'\0';
    } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gRoboInputLen<95) {
        gRoboInput[gRoboInputLen++]=Key.UnicodeChar;
        gRoboInput[gRoboInputLen]=L'\0';
    }
}

VOID RenderTaskManager(UEFI_WINDOW *Win) {

    ResizeTaskManagerToFit(Win);

    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    DrawString(
        L"Processes / Windows",
        X,
        Y,
        COLOR_TEXT
    );

    DrawString(
        L"Lightweight UEFI task manager",
        X,
        Y + 14,
        RGB(148,163,184)
    );

    for (INT32 i = 0; i < TOTAL_WINDOWS; i++) {

        UEFI_WINDOW *W =
            gWinOrder[i];

        if (!W)
            continue;

        INT32 RowY =
            Y + 38 + i * 15;

        BOOLEAN Selected =
            (gTaskMgrSelected == i);

        UINT32 RowBg =
            Selected
                ? RGB(37,99,235)
                : ((i & 1)
                    ? RGB(27,35,49)
                    : RGB(21,28,40));

        DrawRect(
            X,
            RowY,
            Win->W - 20,
            16,
            RowBg
        );

        CONST CHAR16 *State =
            !W->IsVisible
                ? L"CLOSED"
                : (
                    W->IsMinimized
                        ? L"MINIMIZED"
                        : L"RUNNING"
                );

        DrawString(
            W->Title,
            X + 5,
            RowY + 4,
            COLOR_TEXT
        );

        DrawString(
            State,
            X + 190,
            RowY + 4,
            W->IsVisible
                ? NEW_APP_COLOR_GOOD
                : RGB(100,116,139)
        );
    }

    // End Task always sits BELOW the last row.
    DrawButton(
        X,
        Win->Y + Win->H - 42,
        100,
        26,
        L"End Task",
        COLOR_BTN_CLOSE,
        COLOR_TEXT
    );

    DrawString(
        L"Click a row, then End Task.",
        X + 114,
        Win->Y + Win->H - 34,
        RGB(148,163,184)
    );
}

BOOLEAN InputTaskManager(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {

    ResizeTaskManagerToFit(Win);

    if (!Click || Last)
        return TRUE;

    INT32 X =
        Win->X + 10;

    INT32 Y =
        Win->Y + 36;

    // Select process/window row.
    for (INT32 i = 0;
         i < TOTAL_WINDOWS;
         i++) {

        INT32 RowY =
            Y + 38 + i * 15;

        if (
            IsInRect(
                MX,
                MY,
                X,
                RowY,
                Win->W - 20,
                16
            )
        ) {

            gTaskMgrSelected =
                i;

            return TRUE;
        }
    }

    // End Task button.
    if (
        IsInRect(
            MX,
            MY,
            X,
            Win->Y + Win->H - 42,
            100,
            26
        ) &&
        gTaskMgrSelected >= 0 &&
        gTaskMgrSelected < TOTAL_WINDOWS
    ) {

        UEFI_WINDOW *Target =
            gWinOrder[gTaskMgrSelected];

        if (Target != &gTaskMgrWin) {

            Target->IsVisible =
                FALSE;

            Target->IsMinimized =
                FALSE;

            Target->IsDragging =
                FALSE;

            gTaskMgrSelected =
                -1;
        }

        return TRUE;
    }

    return TRUE;
}

// ============================================================
// FILE ARCHIVE (.uarch)
// ============================================================

STATIC EFI_STATUS ReadWholeFile(
    CONST CHAR16 *FileName,
    VOID **Buffer,
    UINTN *BufferSize
) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS = NULL;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_FILE_INFO *Info = NULL;
    UINTN InfoSize = 0;
    EFI_STATUS Status;

    *Buffer = NULL;
    *BufferSize = 0;

    Status = gBS->LocateProtocol(
        &gEfiSimpleFileSystemProtocolGuid,
        NULL,
        (VOID **)&FS
    );

    if (EFI_ERROR(Status))
        return Status;

    Status = FS->OpenVolume(
        FS,
        &Root
    );

    if (EFI_ERROR(Status))
        return Status;

    Status = Root->Open(
        Root,
        &File,
        (CHAR16 *)FileName,
        EFI_FILE_MODE_READ,
        0
    );

    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status;
    }

    Status = File->GetInfo(
        File,
        &gEfiFileInfoGuid,
        &InfoSize,
        NULL
    );

    if (Status == EFI_BUFFER_TOO_SMALL) {
        Status = gBS->AllocatePool(
            EfiLoaderData,
            InfoSize,
            (VOID **)&Info
        );
    }

    if (EFI_ERROR(Status)) {
        File->Close(File);
        Root->Close(Root);
        return Status;
    }

    *BufferSize =
        (UINTN)Info->FileSize;

    Status = gBS->AllocatePool(
        EfiLoaderData,
        *BufferSize + 1,
        Buffer
    );

    if (!EFI_ERROR(Status) &&
        *BufferSize > 0) {

        UINTN ReadSize = *BufferSize;

        Status = File->Read(
            File,
            &ReadSize,
            *Buffer
        );

        if (EFI_ERROR(Status)) {
            gBS->FreePool(*Buffer);
            *Buffer = NULL;
            *BufferSize = 0;
        }
    }

    if (Info)
        gBS->FreePool(Info);

    File->Close(File);
    Root->Close(Root);

    return Status;
}

STATIC EFI_STATUS CreateArchive(VOID) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS = NULL;
    EFI_FILE_PROTOCOL *Root = NULL;

    EFI_STATUS Status =
        gBS->LocateProtocol(
            &gEfiSimpleFileSystemProtocolGuid,
            NULL,
            (VOID **)&FS
        );

    if (EFI_ERROR(Status))
        return Status;

    Status = FS->OpenVolume(
        FS,
        &Root
    );

    if (EFI_ERROR(Status))
        return Status;

    UARCH_HEADER Header;

    Header.Magic = UARCH_MAGIC;
    Header.FileCount = 0;

    VOID *ArchiveBuffer = NULL;
    UINTN ArchiveSize = sizeof(Header);

    UINTN Capacity = 1024 * 1024;

    Status = gBS->AllocatePool(
        EfiLoaderData,
        Capacity,
        &ArchiveBuffer
    );

    if (EFI_ERROR(Status)) {
        Root->Close(Root);
        return Status;
    }

    gBS->CopyMem(
        ArchiveBuffer,
        &Header,
        sizeof(Header)
    );

    Root->SetPosition(
        Root,
        0
    );

    UINTN BufferSize = 1024;
    EFI_FILE_INFO *Info = NULL;

    Status = gBS->AllocatePool(
        EfiLoaderData,
        BufferSize,
        (VOID **)&Info
    );

    if (EFI_ERROR(Status)) {
        gBS->FreePool(ArchiveBuffer);
        Root->Close(Root);
        return Status;
    }

    while (Header.FileCount < UARCH_MAX_FILES) {

        UINTN ReadSize = BufferSize;

        Status = Root->Read(
            Root,
            &ReadSize,
            Info
        );

        if (EFI_ERROR(Status) ||
            ReadSize == 0)
            break;

        if (Info->Attribute & EFI_FILE_DIRECTORY)
            continue;

        if (StrCmp(
            Info->FileName,
            gArchiveName
        ) == 0)
            continue;

        VOID *FileData = NULL;
        UINTN FileSize = 0;

        if (EFI_ERROR(
            ReadWholeFile(
                Info->FileName,
                &FileData,
                &FileSize
            )
        ))
            continue;

        UINTN Needed =
            sizeof(UARCH_ENTRY) +
            FileSize;

        if (ArchiveSize + Needed > Capacity) {
            gBS->FreePool(FileData);
            break;
        }

        UARCH_ENTRY Entry;

        gBS->SetMem(
            &Entry,
            sizeof(Entry),
            0
        );

        StrCpyS(
            Entry.FileName,
            UARCH_NAME_LEN,
            Info->FileName
        );

        Entry.FileSize =
            (UINT32)FileSize;

        gBS->CopyMem(
            (UINT8 *)ArchiveBuffer +
            ArchiveSize,
            &Entry,
            sizeof(Entry)
        );

        ArchiveSize +=
            sizeof(Entry);

        if (FileSize > 0) {
            gBS->CopyMem(
                (UINT8 *)ArchiveBuffer +
                ArchiveSize,
                FileData,
                FileSize
            );

            ArchiveSize +=
                FileSize;
        }

        Header.FileCount++;

        gBS->FreePool(FileData);
    }

    gBS->CopyMem(
        ArchiveBuffer,
        &Header,
        sizeof(Header)
    );

    Status = SaveFileToFS(
        gArchiveName,
        ArchiveBuffer,
        ArchiveSize
    );

    gBS->FreePool(Info);
    gBS->FreePool(ArchiveBuffer);

    Root->Close(Root);

    return Status;
}

VOID RenderArchive(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 38;

    DrawString(
        L"UEFIText File Archive",
        X,
        Y,
        COLOR_TEXT
    );

    DrawString(
        L"Custom .uarch bundle format",
        X,
        Y + 18,
        RGB(148,163,184)
    );

    DrawString(
        L"Bundles files from the EFI root.",
        X,
        Y + 36,
        RGB(100,116,139)
    );

    DrawButton(
        X,
        Y + 65,
        110,
        28,
        L"Create",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawButton(
        X + 120,
        Y + 65,
        110,
        28,
        L"Refresh",
        COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawString(
        gArchiveStatusGood
            ? L"Archive created."
            : L"Ready.",
        X,
        Y + 110,
        gArchiveStatusGood
            ? NEW_APP_COLOR_GOOD
            : COLOR_TEXT
    );

    DrawString(
        L"Output:",
        X,
        Y + 140,
        RGB(148,163,184)
    );

    DrawString(
        gArchiveName,
        X + 60,
        Y + 140,
        COLOR_TEXT
    );
}

BOOLEAN InputArchive(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 38;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X,
        Y + 65,
        110,
        28
    )) {

        if (!EFI_ERROR(
            CreateArchive()
        )) {
            gArchiveStatusGood = TRUE;
            RefreshFileList();
        } else {
            gArchiveStatusGood = FALSE;
        }

        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 120,
        Y + 65,
        110,
        28
    )) {

        RefreshFileList();

        return TRUE;
    }

    return TRUE;
}

// ============================================================
// NOTES APP
// ============================================================

VOID RenderNotes(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;

    DrawRect(
        X,
        Y,
        Win->W - 16,
        28,
        COLOR_TOOLBAR_BG
    );

    DrawButton(
        X + 4,
        Y + 4,
        55,
        20,
        L"New",
        COLOR_WIN_BORDER,
        COLOR_TEXT
    );

    DrawButton(
        X + 64,
        Y + 4,
        55,
        20,
        L"Save",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawButton(
        X + 124,
        Y + 4,
        55,
        20,
        L"Open",
        COLOR_WIN_BORDER,
        COLOR_TEXT
    );

    DrawString(
        gNotesFileName,
        X + 190,
        Y + 10,
        COLOR_TEXT
    );

    INT32 EditorX = X + 6;
    INT32 EditorY = Y + 36;

    DrawRect(
        EditorX,
        EditorY,
        Win->W - 28,
        Win->H - 74,
        COLOR_CANVAS
    );

    INT32 CurX = EditorX + 6;
    INT32 CurY = EditorY + 6;

    for (UINTN i = 0; i < gNotesLen; i++) {

        if (gNotesBuffer[i] == L'\n') {
            CurX = EditorX + 6;
            CurY += 12;
            continue;
        }

        DrawChar(
            gNotesBuffer[i],
            CurX,
            CurY,
            COLOR_TEXT
        );

        CurX += 8;

        if (CurX >= EditorX + Win->W - 42) {
            CurX = EditorX + 6;
            CurY += 12;
        }
    }

    DrawRect(
        CurX,
        CurY,
        8,
        10,
        COLOR_CURSOR
    );

    if (gNotesDirty) {
        DrawString(
            L"Modified",
            X + 6,
            Win->Y + Win->H - 24,
            NEW_APP_COLOR_WARN
        );
    } else {
        DrawString(
            L"Saved",
            X + 6,
            Win->Y + Win->H - 24,
            NEW_APP_COLOR_GOOD
        );
    }
}

BOOLEAN InputNotes(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X + 4,
        Y + 4,
        55,
        20
    )) {
        gNotesBuffer[0] = L'\0';
        gNotesLen = 0;
        StrCpyS(
            gNotesFileName,
            64,
            L"Notes.txt"
        );
        gNotesDirty = FALSE;
        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 64,
        Y + 4,
        55,
        20
    )) {
        SaveFileToFS(
            gNotesFileName,
            gNotesBuffer,
            gNotesLen * sizeof(CHAR16)
        );

        gNotesDirty = FALSE;
        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 124,
        Y + 4,
        55,
        20
    )) {
        RefreshFileList();

        gFileWin.IsVisible = TRUE;
        gFileWin.IsMinimized = FALSE;

        BringToFront(&gFileWin);

        return TRUE;
    }

    return TRUE;
}

// ============================================================
// MINI IDE APP
// ============================================================

STATIC BOOLEAN IDEIsKeyword(CONST CHAR16 *Word) {
    CONST CHAR16 *Keywords[] = {
        L"if",
        L"else",
        L"for",
        L"while",
        L"return",
        L"static",
        L"const",
        L"VOID",
        L"BOOLEAN",
        L"UINT8",
        L"UINT16",
        L"UINT32",
        L"UINTN",
        L"EFI_STATUS",
        L"EFIAPI",
        L"STATIC",
        L"TRUE",
        L"FALSE",
        L"NULL"
    };

    UINTN Count =
        sizeof(Keywords) / sizeof(Keywords[0]);

    for (UINTN i = 0; i < Count; i++) {
        if (StrCmp(Word, Keywords[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

STATIC VOID DrawIDEWord(
    CONST CHAR16 *Word,
    INT32 X,
    INT32 Y
) {
    UINT32 Color =
        IDEIsKeyword(Word)
        ? NEW_APP_COLOR_ACCENT
        : COLOR_TEXT;

    DrawString(
        Word,
        X,
        Y,
        Color
    );
}

VOID RenderMiniIDE(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;

    DrawRect(
        X,
        Y,
        Win->W - 16,
        30,
        COLOR_TOOLBAR_BG
    );

    DrawButton(
        X + 4,
        Y + 5,
        50,
        20,
        L"New",
        COLOR_WIN_BORDER,
        COLOR_TEXT
    );

    DrawButton(
        X + 60,
        Y + 5,
        50,
        20,
        L"Open",
        COLOR_WIN_BORDER,
        COLOR_TEXT
    );

    DrawButton(
        X + 116,
        Y + 5,
        50,
        20,
        L"Save",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawButton(
        X + 172,
        Y + 5,
        50,
        20,
        L"Run",
        NEW_APP_COLOR_GOOD,
        COLOR_TEXT
    );

    DrawString(
        gIDEFileName,
        X + 240,
        Y + 11,
        COLOR_TEXT
    );

    INT32 EditorX = X;
    INT32 EditorY = Y + 34;

    INT32 LineNumberW = 40;

    DrawRect(
        EditorX,
        EditorY,
        Win->W - 16,
        Win->H - 66,
        COLOR_CANVAS
    );

    DrawRect(
        EditorX,
        EditorY,
        LineNumberW,
        Win->H - 66,
        NEW_APP_COLOR_PANEL
    );

    UINTN Line = 1;
    INT32 CurX = EditorX + LineNumberW + 8;
    INT32 CurY = EditorY + 6;

    CHAR16 Word[64];
    UINTN WordLen = 0;

    for (UINTN i = 0; i <= gIDELen; i++) {

        CHAR16 Ch = gIDEBuffer[i];

        BOOLEAN IsWordChar =
            (Ch >= L'a' && Ch <= L'z') ||
            (Ch >= L'A' && Ch <= L'Z') ||
            (Ch >= L'0' && Ch <= L'9') ||
            Ch == L'_';

        if (IsWordChar) {

            if (WordLen < 63)
                Word[WordLen++] = Ch;

        } else {

            if (WordLen > 0) {
                Word[WordLen] = L'\0';

                DrawIDEWord(
                    Word,
                    CurX,
                    CurY
                );

                CurX += (INT32)WordLen * 8;
                WordLen = 0;
            }

            if (Ch == L'\n') {
                CHAR16 LineBuf[16];

                UnicodeSPrint(
                    LineBuf,
                    sizeof(LineBuf),
                    L"%d",
                    Line
                );

                DrawString(
                    LineBuf,
                    EditorX + 6,
                    CurY,
                    RGB(100,116,139)
                );

                Line++;

                CurX = EditorX + LineNumberW + 8;
                CurY += 12;
            }
            else if (Ch != L'\0') {
                DrawChar(
                    Ch,
                    CurX,
                    CurY,
                    COLOR_TEXT
                );

                CurX += 8;
            }
        }
    }

    CHAR16 LastLineBuf[16];

    UnicodeSPrint(
        LastLineBuf,
        sizeof(LastLineBuf),
        L"%d",
        Line
    );

    DrawString(
        LastLineBuf,
        EditorX + 6,
        CurY,
        RGB(100,116,139)
    );

    DrawRect(
        CurX,
        CurY,
        8,
        10,
        COLOR_CURSOR
    );

    if (gIDEDirty) {
        DrawString(
            L"Modified",
            X + 6,
            Win->Y + Win->H - 22,
            NEW_APP_COLOR_WARN
        );
    } else {
        DrawString(
            L"Ready",
            X + 6,
            Win->Y + Win->H - 22,
            NEW_APP_COLOR_GOOD
        );
    }
}

BOOLEAN InputMiniIDE(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X + 4,
        Y + 5,
        50,
        20
    )) {
        gIDEBuffer[0] = L'\0';
        gIDELen = 0;
        gIDECursor = 0;

        StrCpyS(
            gIDEFileName,
            64,
            L"main.c"
        );

        gIDEDirty = FALSE;

        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 60,
        Y + 5,
        50,
        20
    )) {
        UINTN ReadSize = sizeof(gIDEBuffer);

        if (!EFI_ERROR(
            LoadFileFromFS(
                gIDEFileName,
                gIDEBuffer,
                &ReadSize
            )
        )) {
            gIDELen =
                ReadSize / sizeof(CHAR16);

            if (gIDELen >= IDE_MAX_TEXT)
                gIDELen = IDE_MAX_TEXT - 1;

            gIDEBuffer[gIDELen] = L'\0';
            gIDECursor = gIDELen;
            gIDEDirty = FALSE;
        }

        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 116,
        Y + 5,
        50,
        20
    )) {
        SaveFileToFS(
            gIDEFileName,
            gIDEBuffer,
            gIDELen * sizeof(CHAR16)
        );

        gIDEDirty = FALSE;

        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 172,
        Y + 5,
        50,
        20
    )) {
        TerminalPrint(
            L"Mini IDE: Run is not implemented yet."
        );

        return TRUE;
    }

    return TRUE;
}

// ============================================================
// TETRIS APP
// ============================================================

STATIC CONST UINT8 gTetrisPieces[7][4][4][4] = {

    // I
    {
        {
            {0,0,0,0},
            {1,1,1,1},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {0,1,0,0},
            {0,1,0,0},
            {0,1,0,0}
        },
        {
            {0,0,0,0},
            {1,1,1,1},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {0,1,0,0},
            {0,1,0,0},
            {0,1,0,0}
        }
    },

    // O
    {
        {
            {0,1,1,0},
            {0,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,1,0},
            {0,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,1,0},
            {0,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,1,0},
            {0,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        }
    },

    // T
    {
        {
            {0,1,0,0},
            {1,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {0,1,1,0},
            {0,1,0,0},
            {0,0,0,0}
        },
        {
            {0,0,0,0},
            {1,1,1,0},
            {0,1,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {1,1,0,0},
            {0,1,0,0},
            {0,0,0,0}
        }
    },

    // S
    {
        {
            {0,1,1,0},
            {1,1,0,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {1,0,0,0},
            {1,1,0,0},
            {0,1,0,0},
            {0,0,0,0}
        },
        {
            {0,1,1,0},
            {1,1,0,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {1,0,0,0},
            {1,1,0,0},
            {0,1,0,0},
            {0,0,0,0}
        }
    },

    // Z
    {
        {
            {1,1,0,0},
            {0,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {1,1,0,0},
            {1,0,0,0},
            {0,0,0,0}
        },
        {
            {1,1,0,0},
            {0,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {1,1,0,0},
            {1,0,0,0},
            {0,0,0,0}
        }
    },

    // J
    {
        {
            {1,0,0,0},
            {1,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,1,0},
            {0,1,0,0},
            {0,1,0,0},
            {0,0,0,0}
        },
        {
            {1,1,1,0},
            {0,0,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {0,1,0,0},
            {1,1,0,0},
            {0,0,0,0}
        }
    },

    // L
    {
        {
            {0,0,1,0},
            {1,1,1,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {0,1,0,0},
            {0,1,0,0},
            {0,1,1,0},
            {0,0,0,0}
        },
        {
            {1,1,1,0},
            {1,0,0,0},
            {0,0,0,0},
            {0,0,0,0}
        },
        {
            {1,1,0,0},
            {0,1,0,0},
            {0,1,0,0},
            {0,0,0,0}
        }
    }
};

STATIC UINT32 TetrisRandom(VOID) {
    gTetrisRandomState =
        gTetrisRandomState * 1664525u +
        1013904223u;

    return gTetrisRandomState;
}

STATIC UINT32 TetrisCellColor(UINT8 Cell) {
    switch (Cell) {
        case 1: return RGB(34,211,238);
        case 2: return RGB(250,204,21);
        case 3: return RGB(168,85,247);
        case 4: return RGB(34,197,94);
        case 5: return RGB(239,68,68);
        case 6: return RGB(59,130,246);
        case 7: return RGB(249,115,22);
    }

    return COLOR_CANVAS;
}

STATIC BOOLEAN TetrisCollision(
    INT32 PX,
    INT32 PY,
    UINT8 Piece,
    UINT8 Rotation
) {
    for (INT32 y = 0; y < 4; y++) {
        for (INT32 x = 0; x < 4; x++) {

            if (!gTetrisPieces[
                Piece][Rotation][y][x])
                continue;

            INT32 BX = PX + x;
            INT32 BY = PY + y;

            if (BX < 0 ||
                BX >= TETRIS_W ||
                BY >= TETRIS_H)
                return TRUE;

            if (BY >= 0 &&
                gTetrisBoard[BY][BX])
                return TRUE;
        }
    }

    return FALSE;
}

STATIC VOID TetrisMove(INT32 DX) {
    if (gTetrisGameOver)
        return;

    if (!TetrisCollision(
        gTetrisX + DX,
        gTetrisY,
        gTetrisPiece,
        gTetrisRotation
    )) {
        gTetrisX += DX;
    }
}

STATIC VOID TetrisRotate(VOID) {
    if (gTetrisGameOver)
        return;

    UINT8 NewRotation =
        (UINT8)((gTetrisRotation + 1) & 3);

    if (!TetrisCollision(
        gTetrisX,
        gTetrisY,
        gTetrisPiece,
        NewRotation
    )) {
        gTetrisRotation = NewRotation;
    }
}

STATIC VOID TetrisLockPiece(VOID) {
    for (INT32 y = 0; y < 4; y++) {
        for (INT32 x = 0; x < 4; x++) {

            if (!gTetrisPieces[
                gTetrisPiece][gTetrisRotation][y][x])
                continue;

            INT32 BX = gTetrisX + x;
            INT32 BY = gTetrisY + y;

            if (BX >= 0 &&
                BX < TETRIS_W &&
                BY >= 0 &&
                BY < TETRIS_H) {

                gTetrisBoard[BY][BX] =
                    gTetrisPiece + 1;
            }
        }
    }
}

STATIC VOID TetrisClearLines(VOID) {
    UINT32 Cleared = 0;

    for (INT32 y = TETRIS_H - 1; y >= 0; y--) {

        BOOLEAN Full = TRUE;

        for (INT32 x = 0; x < TETRIS_W; x++) {
            if (!gTetrisBoard[y][x]) {
                Full = FALSE;
                break;
            }
        }

        if (Full) {

            for (INT32 yy = y; yy > 0; yy--) {
                for (INT32 x = 0; x < TETRIS_W; x++) {
                    gTetrisBoard[yy][x] =
                        gTetrisBoard[yy - 1][x];
                }
            }

            for (INT32 x = 0; x < TETRIS_W; x++)
                gTetrisBoard[0][x] = 0;

            Cleared++;

            y++;
        }
    }

    if (Cleared) {

        gTetrisLines += Cleared;

        switch (Cleared) {
            case 1:
                gTetrisScore += 100;
                break;

            case 2:
                gTetrisScore += 300;
                break;

            case 3:
                gTetrisScore += 500;
                break;

            case 4:
                gTetrisScore += 800;
                break;
        }

        gTetrisLevel =
            1 + (gTetrisLines / 10);
    }
}

STATIC VOID TetrisSpawn(VOID) {
    gTetrisPiece = gTetrisNextPiece;

    gTetrisNextPiece =
        (UINT8)(TetrisRandom() % 7);

    gTetrisX = 3;
    gTetrisY = 0;
    gTetrisRotation = 0;

    if (TetrisCollision(
        gTetrisX,
        gTetrisY,
        gTetrisPiece,
        gTetrisRotation
    )) {
        gTetrisGameOver = TRUE;
    }
}

VOID ResetTetris(VOID) {

    EFI_TIME TetrisTime;

    gBS->SetMem(
        gTetrisBoard,
        sizeof(gTetrisBoard),
        0
    );

    gTetrisScore = 0;
    gTetrisLines = 0;
    gTetrisLevel = 1;

    gTetrisGameOver = FALSE;

    if (!EFI_ERROR(
        gRT->GetTime(&TetrisTime, NULL)
    )) {
        gTetrisLastSecond = TetrisTime.Second;
    } else {
        gTetrisLastSecond = 0;
    }

    gTetrisPiece =
        (UINT8)(TetrisRandom() % 7);

    gTetrisNextPiece =
        (UINT8)(TetrisRandom() % 7);

    gTetrisX = 3;
    gTetrisY = 0;
    gTetrisRotation = 0;
}

VOID UpdateTetris(VOID) {
    if (gTetrisGameOver)
        return;

    if (!TetrisCollision(
        gTetrisX,
        gTetrisY + 1,
        gTetrisPiece,
        gTetrisRotation
    )) {
        gTetrisY++;
    } else {
        TetrisLockPiece();
        TetrisClearLines();
        TetrisSpawn();
    }
}

VOID RenderTetris(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 38;

    CHAR16 Buf[64];

    UnicodeSPrint(
        Buf,
        sizeof(Buf),
        L"Score: %d",
        gTetrisScore
    );

    DrawString(
        Buf,
        X,
        Y,
        COLOR_TEXT
    );

    UnicodeSPrint(
        Buf,
        sizeof(Buf),
        L"Lines: %d",
        gTetrisLines
    );

    DrawString(
        Buf,
        X + 100,
        Y,
        COLOR_TEXT
    );

    UnicodeSPrint(
        Buf,
        sizeof(Buf),
        L"Level: %d",
        gTetrisLevel
    );

    DrawString(
        Buf,
        X + 200,
        Y,
        COLOR_TEXT
    );

    INT32 Cell = 20;

    INT32 BoardX = X;
    INT32 BoardY = Y + 20;

    DrawRect(
        BoardX - 2,
        BoardY - 2,
        TETRIS_W * Cell + 4,
        TETRIS_H * Cell + 4,
        COLOR_WIN_BORDER
    );

    for (INT32 y = 0; y < TETRIS_H; y++) {
        for (INT32 x = 0; x < TETRIS_W; x++) {

            DrawRect(
                BoardX + x * Cell,
                BoardY + y * Cell,
                Cell - 1,
                Cell - 1,
                TetrisCellColor(
                    gTetrisBoard[y][x]
                )
            );
        }
    }

    if (!gTetrisGameOver) {

        for (INT32 y = 0; y < 4; y++) {
            for (INT32 x = 0; x < 4; x++) {

                if (!gTetrisPieces[
                    gTetrisPiece][gTetrisRotation][y][x])
                    continue;

                INT32 BX = gTetrisX + x;
                INT32 BY = gTetrisY + y;

                if (BX >= 0 &&
                    BX < TETRIS_W &&
                    BY >= 0 &&
                    BY < TETRIS_H) {

                    DrawRect(
                        BoardX + BX * Cell,
                        BoardY + BY * Cell,
                        Cell - 1,
                        Cell - 1,
                        TetrisCellColor(
                            gTetrisPiece + 1
                        )
                    );
                }
            }
        }
    }

    INT32 SideX =
        BoardX + TETRIS_W * Cell + 18;

    DrawString(
        L"NEXT",
        SideX,
        BoardY,
        COLOR_TEXT
    );

    for (INT32 y = 0; y < 4; y++) {
        for (INT32 x = 0; x < 4; x++) {

            if (gTetrisPieces[
                gTetrisNextPiece][0][y][x]) {

                DrawRect(
                    SideX + x * 16,
                    BoardY + 18 + y * 16,
                    15,
                    15,
                    TetrisCellColor(
                        gTetrisNextPiece + 1
                    )
                );
            }
        }
    }

    DrawString(
        L"LEFT / RIGHT",
        SideX,
        BoardY + 100,
        COLOR_TEXT
    );

    DrawString(
        L"UP = ROTATE",
        SideX,
        BoardY + 116,
        COLOR_TEXT
    );

    DrawString(
        L"DOWN = DROP",
        SideX,
        BoardY + 132,
        COLOR_TEXT
    );

    DrawButton(
        X,
        Win->Y + Win->H - 34,
        80,
        24,
        L"Restart",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    if (gTetrisGameOver) {
        DrawString(
            L"GAME OVER",
            BoardX + 42,
            BoardY + 180,
            RGB(239,68,68)
        );
    }
}

BOOLEAN InputTetris(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    if (Click && !Last) {

        if (IsInRect(
            MX,
            MY,
            Win->X + 12,
            Win->Y + Win->H - 34,
            80,
            24
        )) {
            ResetTetris();
        }
    }

    return TRUE;
}

VOID RenderImageViewer(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;

    DrawButton(
        X,
        Y,
        65,
        22,
        L"Open",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawButton(
        X + 70,
        Y,
        65,
        22,
        L"1x",
        COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        X + 140,
        Y,
        65,
        22,
        L"2x",
        COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawString(
        gImageViewerFileName,
        X + 220,
        Y + 7,
        COLOR_TEXT
    );

    INT32 FrameX = X + 10;
    INT32 FrameY = Y + 34;

    DrawRect(
        FrameX - 2,
        FrameY - 2,
        PAINT_W + 4,
        PAINT_H + 4,
        COLOR_WIN_BORDER
    );

    if (!gImageViewerLoaded) {
        DrawString(
            L"No image loaded.",
            FrameX + 65,
            FrameY + 80,
            RGB(148,163,184)
        );

        DrawString(
            L"Open a .pnt file.",
            FrameX + 65,
            FrameY + 100,
            RGB(100,116,139)
        );

        return;
    }

    for (INT32 py = 0; py < PAINT_H; py++) {
        for (INT32 px = 0; px < PAINT_W; px++) {

            UINT32 C =
                gImageViewerCanvas[
                    py * PAINT_W + px
                ];

            if (gImageViewerZoom == 1) {

                FastDrawPixel(
                    FrameX + px,
                    FrameY + py,
                    C
                );

            } else {

                DrawRect(
                    FrameX + px * 2,
                    FrameY + py * 2,
                    2,
                    2,
                    C
                );
            }
        }
    }
}

BOOLEAN InputImageViewer(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(MX,MY,X,Y,65,22)) {
        RefreshFileList();

        gFileWin.IsVisible = TRUE;
        gFileWin.IsMinimized = FALSE;

        BringToFront(&gFileWin);

        return TRUE;
    }

    if (IsInRect(MX,MY,X+70,Y,65,22))
        gImageViewerZoom = 1;

    if (IsInRect(MX,MY,X+140,Y,65,22))
        gImageViewerZoom = 2;

    return TRUE;
}

// ============================================================
// COMMON APPS
// ============================================================

STATIC BOOLEAN IsLeapYear(UINT16 Year) {
    return (
        ((Year % 4) == 0 && (Year % 100) != 0) ||
        ((Year % 400) == 0)
    );
}

STATIC UINT8 DaysInMonth(
    UINT16 Year,
    UINT8 Month
) {
    STATIC CONST UINT8 Days[] = {
        31,28,31,30,31,30,
        31,31,30,31,30,31
    };

    if (Month == 2 &&
        IsLeapYear(Year))
        return 29;

    if (Month >= 1 &&
        Month <= 12)
        return Days[Month - 1];

    return 30;
}

VOID RenderCommonApps(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    DrawButton(
        X,
        Y,
        82,
        24,
        L"Calendar",
        gCommonAppTab == COMMON_APP_CALENDAR
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        X + 86,
        Y,
        82,
        24,
        L"Clock",
        gCommonAppTab == COMMON_APP_CLOCK
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        X + 172,
        Y,
        82,
        24,
        L"Stopwatch",
        gCommonAppTab == COMMON_APP_STOPWATCH
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        X + 258,
        Y,
        82,
        24,
        L"About",
        gCommonAppTab == COMMON_APP_ABOUT
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    INT32 ContentY = Y + 38;

    DrawRect(
        X,
        ContentY,
        Win->W - 20,
        Win->H - 86,
        COLOR_CANVAS
    );

    // ========================================================
    // CALENDAR
    // ========================================================

    if (gCommonAppTab == COMMON_APP_CALENDAR) {

        EFI_TIME Time;

        if (!gCalendarInitialized) {
            if (!EFI_ERROR(
                gRT->GetTime(&Time, NULL)
            )) {
                gCalendarYear = Time.Year;
                gCalendarMonth = Time.Month;
                gCalendarSelectedDay = Time.Day;
            }

            gCalendarInitialized = TRUE;
        }

        CHAR16 Header[64];

        UnicodeSPrint(
            Header,
            sizeof(Header),
            L"%04d / %02d",
            gCalendarYear,
            gCalendarMonth
        );

        DrawString(
            Header,
            X + 90,
            ContentY + 8,
            COLOR_TEXT
        );

        DrawButton(
            X + 10,
            ContentY + 4,
            55,
            22,
            L"<",
            COLOR_TOOLBAR_BG,
            COLOR_TEXT
        );

        DrawButton(
            X + 230,
            ContentY + 4,
            55,
            22,
            L">",
            COLOR_TOOLBAR_BG,
            COLOR_TEXT
        );

        CONST CHAR16 *Days[] = {
            L"Su",
            L"Mo",
            L"Tu",
            L"We",
            L"Th",
            L"Fr",
            L"Sa"
        };

        INT32 GridX = X + 12;
        INT32 GridY = ContentY + 38;

        for (INT32 i = 0; i < 7; i++) {
            DrawString(
                Days[i],
                GridX + i * 36,
                GridY,
                RGB(148,163,184)
            );
        }

        // January 1, 2026 was Thursday.
        // Calculate weekday for any date.
        UINT16 YR = gCalendarYear;
        UINT8 MO = gCalendarMonth;
        UINT8 D = 1;

        INT32 WDay = 0;

        UINT16 yy = YR;
        UINT8 mm = MO;

        if (mm < 3) {
            mm += 12;
            yy--;
        }

        WDay = (
            D +
            (13 * (mm + 1)) / 5 +
            yy +
            yy / 4 -
            yy / 100 +
            yy / 400
        ) % 7;

        // Zeller produces:
        // 0 = Saturday, 1 = Sunday, etc.
        WDay = (WDay + 6) % 7;

        UINT8 MonthDays =
            DaysInMonth(
                gCalendarYear,
                gCalendarMonth
            );

        for (UINT8 Day = 1;
             Day <= MonthDays;
             Day++) {

            INT32 Pos =
                WDay + Day - 1;

            INT32 Col = Pos % 7;
            INT32 Row = Pos / 7;

            INT32 DX =
                GridX + Col * 36;

            INT32 DY =
                GridY + 18 + Row * 25;

            UINT32 BG =
                (Day == gCalendarSelectedDay)
                    ? COLOR_TITLE_ACTIVE
                    : COLOR_TOOLBAR_BG;

            DrawRect(
                DX,
                DY,
                28,
                20,
                BG
            );

            CHAR16 DayBuf[8];

            UnicodeSPrint(
                DayBuf,
                sizeof(DayBuf),
                L"%d",
                Day
            );

            DrawString(
                DayBuf,
                DX + 10,
                DY + 6,
                COLOR_TEXT
            );
        }
    }

    // ========================================================
    // CLOCK
    // ========================================================

    else if (gCommonAppTab == COMMON_APP_CLOCK) {

        EFI_TIME Time;

        if (!EFI_ERROR(
            gRT->GetTime(&Time, NULL)
        )) {

            CHAR16 Buf[32];

            UnicodeSPrint(
                Buf,
                sizeof(Buf),
                L"%02d:%02d:%02d",
                Time.Hour,
                Time.Minute,
                Time.Second
            );

            DrawString(
                Buf,
                X + 35,
                ContentY + 45,
                NEW_APP_COLOR_GOOD
            );

            UnicodeSPrint(
                Buf,
                sizeof(Buf),
                L"%02d/%02d/%04d",
                Time.Day,
                Time.Month,
                Time.Year
            );

            DrawString(
                Buf,
                X + 75,
                ContentY + 75,
                COLOR_TEXT
            );

            DrawString(
                L"UEFI Real-Time Clock",
                X + 72,
                ContentY + 105,
                RGB(148,163,184)
            );
        }
    }

    // ========================================================
    // STOPWATCH
    // ========================================================

    else if (gCommonAppTab == COMMON_APP_STOPWATCH) {

        CHAR16 Buf[32];

        UINT32 Seconds =
            gStopwatchTicks;

        UINT32 Minutes =
            Seconds / 60;

        Seconds %= 60;

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%02d:%02d",
            Minutes,
            Seconds
        );

        DrawString(
            Buf,
            X + 30,
            ContentY + 20,
            NEW_APP_COLOR_GOOD
        );

        DrawButton(
            X + 20,
            ContentY + 60,
            80,
            24,
            gStopwatchRunning
                ? L"Stop"
                : L"Start",
            gStopwatchRunning
                ? COLOR_BTN_CLOSE
                : NEW_APP_COLOR_GOOD,
            COLOR_TEXT
        );

        DrawButton(
            X + 108,
            ContentY + 60,
            80,
            24,
            L"Reset",
            COLOR_WIN_BORDER,
            COLOR_TEXT
        );
    }

    // ========================================================
    // ABOUT
    // ========================================================

    else {

        DrawString(
            L"UEFIText",
            X + 12,
            ContentY + 12,
            NEW_APP_COLOR_ACCENT
        );

        DrawString(
            L"Native UEFI desktop environment",
            X + 12,
            ContentY + 36,
            COLOR_TEXT
        );

        DrawString(
            L"x64 / GOP / UEFI",
            X + 12,
            ContentY + 54,
            RGB(148,163,184)
        );

        DrawString(
            L"Made in UEFI C",
            X + 12,
            ContentY + 72,
            RGB(148,163,184)
        );

        DrawString(
            L"Version 1.0",
            X + 12,
            ContentY + 90,
            RGB(148,163,184)
        );
    }
}

BOOLEAN InputCommonApps(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(MX,MY,X,Y,82,24)) {
        gCommonAppTab =
            COMMON_APP_CALENDAR;
        return TRUE;
    }

    if (IsInRect(MX,MY,X+86,Y,82,24)) {
        gCommonAppTab =
            COMMON_APP_CLOCK;
        return TRUE;
    }

    if (IsInRect(MX,MY,X+172,Y,82,24)) {
        gCommonAppTab =
            COMMON_APP_STOPWATCH;
        return TRUE;
    }

    if (IsInRect(MX,MY,X+258,Y,82,24)) {
        gCommonAppTab =
            COMMON_APP_ABOUT;
        return TRUE;
    }

    if (gCommonAppTab ==
        COMMON_APP_CALENDAR) {

        INT32 ContentY = Y + 38;

        if (IsInRect(
            MX,
            MY,
            X + 10,
            ContentY + 4,
            55,
            22
        )) {

            if (gCalendarMonth == 1) {
                gCalendarMonth = 12;
                gCalendarYear--;
            } else {
                gCalendarMonth--;
            }

            UINT8 MaxDay =
                DaysInMonth(
                    gCalendarYear,
                    gCalendarMonth
                );

            if (gCalendarSelectedDay > MaxDay)
                gCalendarSelectedDay = MaxDay;

            return TRUE;
        }

        if (IsInRect(
            MX,
            MY,
            X + 230,
            ContentY + 4,
            55,
            22
        )) {

            if (gCalendarMonth == 12) {
                gCalendarMonth = 1;
                gCalendarYear++;
            } else {
                gCalendarMonth++;
            }

            UINT8 MaxDay =
                DaysInMonth(
                    gCalendarYear,
                    gCalendarMonth
                );

            if (gCalendarSelectedDay > MaxDay)
                gCalendarSelectedDay = MaxDay;

            return TRUE;
        }

        // Day selection
        UINT16 YR = gCalendarYear;
        UINT8 MO = gCalendarMonth;

        UINT16 yy = YR;
        UINT8 mm = MO;

        if (mm < 3) {
            mm += 12;
            yy--;
        }

        INT32 WDay = (
            1 +
            (13 * (mm + 1)) / 5 +
            yy +
            yy / 4 -
            yy / 100 +
            yy / 400
        ) % 7;

        WDay = (WDay + 6) % 7;

        INT32 GridX = X + 12;
        INT32 GridY = ContentY + 38;

        for (UINT8 Day = 1;
             Day <= DaysInMonth(
                 gCalendarYear,
                 gCalendarMonth
             );
             Day++) {

            INT32 Pos =
                WDay + Day - 1;

            INT32 Col = Pos % 7;
            INT32 Row = Pos / 7;

            INT32 DX =
                GridX + Col * 36;

            INT32 DY =
                GridY + 18 + Row * 25;

            if (IsInRect(
                MX,
                MY,
                DX,
                DY,
                28,
                20
            )) {
                gCalendarSelectedDay = Day;
                return TRUE;
            }
        }
    }

    if (gCommonAppTab ==
        COMMON_APP_STOPWATCH) {

        INT32 ContentY = Y + 38;

        if (IsInRect(
            MX,
            MY,
            X + 20,
            ContentY + 60,
            80,
            24
        )) {

            gStopwatchRunning =
                !gStopwatchRunning;

            if (gStopwatchRunning) {
                EFI_TIME StartTime;

                if (!EFI_ERROR(
                    gRT->GetTime(
                        &StartTime,
                        NULL
                    )
                )) {
                    gStopwatchLastSecond =
                        StartTime.Second;
                }
            }

            return TRUE;
        }

        if (IsInRect(
            MX,
            MY,
            X + 108,
            ContentY + 60,
            80,
            24
        )) {

            gStopwatchTicks = 0;
            gStopwatchRunning = FALSE;

            EFI_TIME ResetTime;

            if (!EFI_ERROR(
                gRT->GetTime(
                    &ResetTime,
                    NULL
                )
            )) {
                gStopwatchLastSecond =
                    ResetTime.Second;
            }

            return TRUE;
        }
    }

    return TRUE;
}

// ============================================================
// CLIPBOARD
// ============================================================

STATIC VOID ClipboardAdd(
    CONST CHAR16 *Text
) {
    if (!Text || Text[0] == L'\0')
        return;

    if (gClipboardCount >= CLIPBOARD_HISTORY) {
        for (UINTN i = 0;
             i < CLIPBOARD_HISTORY - 1;
             i++) {

            gBS->CopyMem(
                gClipboardHistory[i],
                gClipboardHistory[i + 1],
                sizeof(gClipboardHistory[i])
            );
        }

        gClipboardCount =
            CLIPBOARD_HISTORY - 1;
    }

    StrCpyS(
        gClipboardHistory[gClipboardCount],
        CLIPBOARD_TEXT_MAX,
        Text
    );

    gClipboardCount++;
    gClipboardSelected =
        (INT32)gClipboardCount - 1;
}

VOID RenderClipboard(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    DrawButton(
        X,
        Y,
        120,
        24,
        L"Copy Editor",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawButton(
        X + 125,
        Y,
        120,
        24,
        L"Copy Notes",
        COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        X + 250,
        Y,
        80,
        24,
        L"Clear",
        COLOR_BTN_CLOSE,
        COLOR_TEXT
    );

    DrawString(
        L"Clipboard History",
        X,
        Y + 40,
        COLOR_TEXT
    );

    for (
        UINTN i = 0;
        i < gClipboardCount;
        i++
    ) {
        INT32 RowY =
            Y + 60 + (INT32)i * 28;

        UINT32 BG =
            ((INT32)i == gClipboardSelected)
                ? COLOR_TITLE_ACTIVE
                : COLOR_TOOLBAR_BG;

        DrawRect(
            X,
            RowY,
            Win->W - 20,
            23,
            BG
        );

        DrawString(
            gClipboardHistory[i],
            X + 6,
            RowY + 7,
            COLOR_TEXT
        );
    }
}

BOOLEAN InputClipboard(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X,
        Y,
        120,
        24
    )) {
        ClipboardAdd(
            gTextBuffer
        );

        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 125,
        Y,
        120,
        24
    )) {
        ClipboardAdd(
            gNotesBuffer
        );

        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 250,
        Y,
        80,
        24
    )) {
        gClipboardCount = 0;
        gClipboardSelected = -1;

        return TRUE;
    }

    for (
        UINTN i = 0;
        i < gClipboardCount;
        i++
    ) {
        INT32 RowY =
            Y + 60 + (INT32)i * 28;

        if (IsInRect(
            MX,
            MY,
            X,
            RowY,
            Win->W - 20,
            23
        )) {
            gClipboardSelected = (INT32)i;
            return TRUE;
        }
    }

    return TRUE;
}

// ============================================================
// UNIT CONVERTER
// ============================================================

VOID RenderUnitConverter(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    DrawButton(
        X,
        Y,
        100,
        24,
        L"Bytes",
        gUnitMode == UNIT_BYTES
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        X + 105,
        Y,
        100,
        24,
        L"Temp",
        gUnitMode == UNIT_TEMPERATURE
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawRect(
        X,
        Y + 40,
        220,
        26,
        COLOR_CANVAS
    );

    DrawString(
        gUnitInput,
        X + 8,
        Y + 48,
        COLOR_TEXT
    );

    if (gUnitMode == UNIT_BYTES) {

        UINT64 Value =
            StrDecimalToUintn(
                gUnitInput
            );

        CHAR16 Buf[64];

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%d bytes",
            (UINT32)Value
        );

        DrawString(
            Buf,
            X,
            Y + 82,
            COLOR_TEXT
        );

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%d KB",
            (UINT32)(Value / 1024)
        );

        DrawString(
            Buf,
            X,
            Y + 100,
            NEW_APP_COLOR_ACCENT
        );

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%d MB",
            (UINT32)(Value / (1024 * 1024))
        );

        DrawString(
            Buf,
            X,
            Y + 118,
            NEW_APP_COLOR_GOOD
        );

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%d GB",
            (UINT32)(Value /
                (1024 * 1024 * 1024))
        );

        DrawString(
            Buf,
            X,
            Y + 136,
            NEW_APP_COLOR_WARN
        );

    } else {

        INT32 C =
            (INT32)StrDecimalToUintn(
                gUnitInput
            );

        INT32 F =
            (C * 9) / 5 + 32;

        INT32 BackC =
            ((C - 32) * 5) / 9;

        CHAR16 Buf[64];

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%d C = %d F",
            C,
            F
        );

        DrawString(
            Buf,
            X,
            Y + 86,
            COLOR_TEXT
        );

        UnicodeSPrint(
            Buf,
            sizeof(Buf),
            L"%d F -> %d C",
            C,
            BackC
        );

        DrawString(
            Buf,
            X,
            Y + 108,
            NEW_APP_COLOR_ACCENT
        );
    }

    DrawString(
        L"Type number with keyboard.",
        X,
        Y + 170,
        RGB(100,116,139)
    );
}

BOOLEAN InputUnitConverter(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X,
        Y,
        100,
        24
    )) {
        gUnitMode = UNIT_BYTES;
        return TRUE;
    }

    if (IsInRect(
        MX,
        MY,
        X + 105,
        Y,
        100,
        24
    )) {
        gUnitMode = UNIT_TEMPERATURE;
        return TRUE;
    }

    return TRUE;
}

// ============================================================
// FILE SEARCH
// ============================================================

STATIC BOOLEAN SearchContains(
    CONST CHAR16 *Text,
    CONST CHAR16 *Query
) {
    if (!Query ||
        Query[0] == L'\0')
        return FALSE;

    for (UINTN i = 0; Text[i]; i++) {

        UINTN j = 0;

        while (
            Text[i + j] &&
            Query[j] &&
            Text[i + j] == Query[j]
        ) {
            j++;
        }

        if (Query[j] == L'\0')
            return TRUE;
    }

    return FALSE;
}

STATIC VOID ExecuteFileSearch(VOID) {
    RefreshFileList();

    gSearchResultCount = 0;

    for (
        UINTN i = 0;
        i < gFileCount &&
        gSearchResultCount < MAX_FILES;
        i++
    ) {

        if (SearchContains(
            gFileList[i],
            gSearchQuery
        )) {

            StrCpyS(
                gSearchResults[
                    gSearchResultCount
                ],
                64,
                gFileList[i]
            );

            gSearchResultCount++;
        }
    }

    gSearchDone = TRUE;
}

VOID RenderSearch(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    DrawRect(
        X,
        Y,
        360,
        26,
        COLOR_CANVAS
    );

    DrawString(
        gSearchQuery,
        X + 7,
        Y + 8,
        COLOR_TEXT
    );

    DrawButton(
        X + 370,
        Y,
        80,
        26,
        L"Search",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawString(
        gSearchDone
            ? L"Results:"
            : L"Enter filename to search",
        X,
        Y + 42,
        COLOR_TEXT
    );

    for (
        UINTN i = 0;
        i < gSearchResultCount;
        i++
    ) {
        DrawRect(
            X,
            Y + 62 + (INT32)i * 22,
            360,
            20,
            COLOR_TOOLBAR_BG
        );

        DrawString(
            gSearchResults[i],
            X + 6,
            Y + 68 + (INT32)i * 22,
            COLOR_TEXT
        );
    }
}

BOOLEAN InputSearch(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X + 370,
        Y,
        80,
        26
    )) {
        ExecuteFileSearch();
        return TRUE;
    }

    return TRUE;
}

// ============================================================
// PC SPEAKER MUSIC
// ============================================================

typedef struct {
    UINT32 Frequency;
    UINT32 Duration;
} MUSIC_NOTE;

STATIC CONST MUSIC_NOTE gMusicSong0[] = {
    {523,120},
    {659,120},
    {784,120},
    {1047,220},
    {784,120},
    {659,120},
    {523,220},
    {0,80},

    {659,120},
    {784,120},
    {1047,180},
    {988,120},
    {784,120},
    {659,220},
    {0,100}
};

VOID PlayMusicSong(UINTN Song) {
    if (Song == 0) {

        UINTN Count =
            sizeof(gMusicSong0) /
            sizeof(gMusicSong0[0]);

        gMusicPlaying = TRUE;

        for (
            UINTN i = 0;
            i < Count;
            i++
        ) {
            PlayTone(
                gMusicSong0[i].Frequency,
                gMusicSong0[i].Duration
            );
        }

        gMusicPlaying = FALSE;
    }
}

VOID RenderMusic(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 40;

    DrawString(
        L"PC Speaker Music",
        X,
        Y,
        COLOR_TEXT
    );

    DrawString(
        L"Tiny UEFIText melody",
        X,
        Y + 20,
        RGB(148,163,184)
    );

    DrawButton(
        X,
        Y + 55,
        100,
        28,
        L"Play",
        COLOR_TITLE_ACTIVE,
        COLOR_TEXT
    );

    DrawString(
        gMusicPlaying
            ? L"Playing..."
            : L"Ready.",
        X,
        Y + 100,
        gMusicPlaying
            ? NEW_APP_COLOR_GOOD
            : COLOR_TEXT
    );

    DrawString(
        L"Uses the PC speaker through PIT.",
        X,
        Y + 135,
        RGB(100,116,139)
    );
}

BOOLEAN InputMusic(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 40;

    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        X,
        Y + 55,
        100,
        28
    )) {
        PlayMusicSong(0);

        return TRUE;
    }

    return TRUE;
}

STATIC VOID StartJumpscare(VOID) {

    gJumpscareActive = TRUE;

    gJumpscareElapsedMs = 0;

    gJumpscareRandom ^= 0x7A91C3E5;

    gJumpscareSoundPending = TRUE;

    gJumpscareMessage = FALSE;
    gJumpscareMessageTimer = 0;
}

STATIC VOID UpdateJumpscare(VOID) {

    if (gJumpscareMessage) {

        if (gJumpscareMessageTimer > 0) {
            gJumpscareMessageTimer--;

            if (gJumpscareMessageTimer == 0) {

                gJumpscareMessage = FALSE;

                // =================================================
                // FORCE SHUTDOWN AFTER SECRET MESSAGE
                // =================================================

                gRT->ResetSystem(
                    EfiResetShutdown,
                    EFI_SUCCESS,
                    0,
                    NULL
                );
            }
        }

        return;
    }

    if (!gJumpscareActive)
        return;

    /*
     * Main loop stalls for roughly 10ms.
     * Count elapsed milliseconds here.
     *
     * The random beeps can add a little extra time,
     * which is fine for the effect.
     */

    gJumpscareElapsedMs += 10;

    /*
     * Make another random beep constantly.
     */

    if ((JumpscareRandom() % 7) == 0) {
        gJumpscareSoundPending = TRUE;
    }

    /*
     * THREE SECOND JUMPSCARE.
     */

    if (gJumpscareElapsedMs >= 3000) {

        gJumpscareActive = FALSE;

        gJumpscareSoundPending = FALSE;

        /*
         * Show the hidden Base16 message AFTER
         * the actual jumpscare, not during it.
         */

        gJumpscareMessage = TRUE;

        /*
         * Roughly one second of message display.
         */

        gJumpscareMessageTimer = 150;
    }
    
    /*
    // this is the end.. THIS IS THE ENDDD!!!!
    gRT->ResetSystem(
    EfiResetShutdown,
    EFI_SUCCESS,
    0,
    NULL
    );
    */
}

STATIC VOID PlayJumpscareBeep(VOID) {

    UINT32 Frequency;

    switch (JumpscareRandom() % 8) {

        case 0:
            Frequency = 80;
            break;

        case 1:
            Frequency = 130;
            break;

        case 2:
            Frequency = 220;
            break;

        case 3:
            Frequency = 440;
            break;

        case 4:
            Frequency = 660;
            break;

        case 5:
            Frequency = 900;
            break;

        case 6:
            Frequency = 1400;
            break;

        default:
            Frequency = 1900;
            break;
    }

    UINT32 Duration =
        15 +
        (JumpscareRandom() % 70);

    PlayTone(
        Frequency,
        Duration
    );
}

STATIC VOID RenderJumpscare(VOID) {

    /*
     * COMPLETE RANDOM GLITCH SCREEN
     *
     * The entire framebuffer is regenerated every frame.
     */

    INT32 TileW = 16;
    INT32 TileH = 16;

    for (
        INT32 Y = 0;
        Y < (INT32)gScreenHeight;
        Y += TileH
    ) {

        for (
            INT32 X = 0;
            X < (INT32)gScreenWidth;
            X += TileW
        ) {

            UINT32 R =
                JumpscareRandom() & 0xFF;

            UINT32 G =
                JumpscareRandom() & 0xFF;

            UINT32 B =
                JumpscareRandom() & 0xFF;

            /*
             * Sometimes make a tile nearly black.
             * Sometimes nearly white.
             * Sometimes completely chaotic.
             */

            UINT32 Mode =
                JumpscareRandom() % 6;

            if (Mode == 0) {
                R = 0;
                G = 0;
                B = 0;
            }
            else if (Mode == 1) {
                R = 255;
                G = 255;
                B = 255;
            }
            else if (Mode == 2) {
                R = 255;
                G = 0;
                B = 0;
            }
            else if (Mode == 3) {
                R = 0;
                G = 255;
                B = 0;
            }
            else if (Mode == 4) {
                R = 0;
                G = 0;
                B = 255;
            }

            DrawRect(
                X,
                Y,
                TileW,
                TileH,
                RGB(R,G,B)
            );
        }
    }

    /*
     * Random horizontal glitch bars.
     */

    for (INT32 i = 0; i < 24; i++) {

        INT32 Y =
            (INT32)(
                JumpscareRandom()
                % gScreenHeight
            );

        INT32 H =
            1 +
            (INT32)(
                JumpscareRandom()
                % 14
            );

        INT32 X =
            (INT32)(
                JumpscareRandom()
                % gScreenWidth
            );

        INT32 W =
            20 +
            (INT32)(
                JumpscareRandom()
                % (gScreenWidth / 2)
            );

        UINT32 Color =
            RGB(
                JumpscareRandom() & 0xFF,
                JumpscareRandom() & 0xFF,
                JumpscareRandom() & 0xFF
            );

        DrawRect(
            X,
            Y,
            W,
            H,
            Color
        );
    }

    /*
     * Occasionally flash the ENTIRE screen.
     */

    if ((JumpscareRandom() % 9) == 0) {

        UINT32 FlashColor;

        switch (JumpscareRandom() % 5) {

            case 0:
                FlashColor = RGB(255,0,0);
                break;

            case 1:
                FlashColor = RGB(0,255,0);
                break;

            case 2:
                FlashColor = RGB(0,0,255);
                break;

            case 3:
                FlashColor = RGB(255,255,255);
                break;

            default:
                FlashColor = RGB(0,0,0);
                break;
        }

        DrawRect(
            0,
            0,
            gScreenWidth,
            gScreenHeight,
            FlashColor
        );
    }
}

STATIC VOID RenderJumpscareMessage(VOID) {

    DrawRect(
        0,
        0,
        gScreenWidth,
        gScreenHeight,
        RGB(0,0,0)
    );

    INT32 X = 20;
    INT32 Y = 30;

    DrawString(
        L"UEFIText secret message:",
        X,
        Y,
        COLOR_TEXT
    );

    Y += 24;

    UINTN Count =
        sizeof(gJumpscareHexMessage) /
        sizeof(gJumpscareHexMessage[0]);

    for (UINTN i = 0; i < Count; i++) {

        DrawString(
            gJumpscareHexMessage[i],
            X,
            Y,
            RGB(100,255,100)
        );

        Y += 12;
    }

    DrawString(
        L"bye bye but the text is base16 hope it helps ;D n' ._.",
        X,
        Y + 12,
        RGB(100,150,150)
    );
}

// ============================================================
// EASTER EGG APP
// ============================================================

VOID RenderEasterEgg(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 20;
    INT32 Y = Win->Y + 44;

    DrawString(
        L"CONGRATULATIONS.",
        X,
        Y,
        NEW_APP_COLOR_GOOD
    );

    DrawString(
        L"You found the forbidden UEFIText room.",
        X,
        Y + 24,
        COLOR_TEXT
    );

    DrawString(
        L"._.",
        X + 150,
        Y + 65,
        RGB(255,255,255)
    );

    DrawString(
        L"Nothing important is here.",
        X,
        Y + 105,
        RGB(100,116,139)
    );

    DrawString(
        L"...probably.",
        X,
        Y + 125,
        RGB(100,116,139)
    );

    DrawString(
        L"maybe do something else :D",
        X,
        Y + 95,
        RGB(255,155,255)
    );

    DrawButton(
        X + 105,
        Y + 160,
        100,
        25,
        L"Leave",
        COLOR_BTN_CLOSE,
        COLOR_TEXT
    );
}

BOOLEAN InputEasterEgg(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    if (!Click || Last)
        return TRUE;

    if (IsInRect(
        MX,
        MY,
        Win->X + 125,
        Win->Y + 44 + 160,
        100,
        25
    )) {

        gEasterEggWin.IsVisible = FALSE;

        StartJumpscare();

        return TRUE;
    }

    return TRUE;
}



// --- Window Input ---
VOID PollMouse(BOOLEAN *OutLeftClick) {
    BOOLEAN Clicked=FALSE;
    STATIC UINT64 LastAbsX=0,LastAbsY=0;

    for (UINTN i=0;i<gNumMouseDevices;i++) {
        EFI_SIMPLE_POINTER_STATE State;

        if (!EFI_ERROR(gMouseDevices[i]->GetState(gMouseDevices[i],&State))) {
            INT32 dx=(INT32)State.RelativeMovementX;
            INT32 dy=(INT32)State.RelativeMovementY;

            if (gMouseDevices[i]->Mode&&gMouseDevices[i]->Mode->ResolutionX>0)
                dx=(INT32)((State.RelativeMovementX*8)/(INT64)gMouseDevices[i]->Mode->ResolutionX);

            if (gMouseDevices[i]->Mode&&gMouseDevices[i]->Mode->ResolutionY>0)
                dy=(INT32)((State.RelativeMovementY*8)/(INT64)gMouseDevices[i]->Mode->ResolutionY);

            gMouseX+=dx;
            gMouseY+=dy;

            if (State.LeftButton) Clicked=TRUE;
        }
    }

    if (gAbsMouse) {
        EFI_ABSOLUTE_POINTER_STATE AbsState;

        if (!EFI_ERROR(gAbsMouse->GetState(gAbsMouse,&AbsState))) {
            if (gAbsMouse->Mode&&gAbsMouse->Mode->AbsoluteMaxX>0) {
                if (AbsState.CurrentX!=LastAbsX||AbsState.CurrentY!=LastAbsY||AbsState.ActiveButtons!=0) {
                    gMouseX=(INT32)((AbsState.CurrentX*gScreenWidth)/gAbsMouse->Mode->AbsoluteMaxX);
                    gMouseY=(INT32)((AbsState.CurrentY*gScreenHeight)/gAbsMouse->Mode->AbsoluteMaxY);
                    LastAbsX=AbsState.CurrentX;
                    LastAbsY=AbsState.CurrentY;
                }
            }

            if (AbsState.ActiveButtons&(EFI_ABSPTR_TOUCH_OR_FIRST_BUTTON|EFI_ABSP_TouchActive))
                Clicked=TRUE;
        }
    }

    if (gMouseX<0) gMouseX=0;
    if (gMouseY<0) gMouseY=0;
    if (gMouseX>=(INT32)gScreenWidth) gMouseX=gScreenWidth-1;
    if (gMouseY>=(INT32)gScreenHeight) gMouseY=gScreenHeight-1;

    if (OutLeftClick) *OutLeftClick=Clicked;
}

VOID PollKeyboard(VOID) {
    EFI_INPUT_KEY Key;

    if (gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)!=EFI_SUCCESS) return;

    // ============================================================
    // SECRET EASTER EGG DETECTOR
    // ============================================================

    if (!gEasterEggUnlocked) {

        BOOLEAN TextAppBusy =
            (gNotesWin.IsVisible &&
             gWinOrder[TOTAL_WINDOWS-1] == &gNotesWin) ||
            (gMiniIDEWin.IsVisible &&
             gWinOrder[TOTAL_WINDOWS-1] == &gMiniIDEWin) ||
            (gEditorWin.IsVisible &&
             gWinOrder[TOTAL_WINDOWS-1] == &gEditorWin) ||
            (gTextTermWin.IsVisible &&
             gWinOrder[TOTAL_WINDOWS-1] == &gTextTermWin);

        if (!TextAppBusy &&
            Key.UnicodeChar >= 0x20 &&
            Key.UnicodeChar <= 0x7E) {

            CHAR16 Ch =
                Key.UnicodeChar;

            if (Ch ==
                gEasterSequence[
                    gEasterSequencePos
                ]) {

                gEasterSequencePos++;

                if (gEasterSequence[
                    gEasterSequencePos
                ] == L'\0') {

                    gEasterEggUnlocked = TRUE;

                    gEasterEggWin.IsVisible = TRUE;
                    gEasterEggWin.IsMinimized = FALSE;

                    BringToFront(
                        &gEasterEggWin
                    );

                    gEasterSequencePos = 0;
                }

            } else {

                gEasterSequencePos = 0;

                if (Ch == gEasterSequence[0])
                    gEasterSequencePos = 1;
            }
        }
    }

    // ========================================================
    // TETRIS KEYBOARD INPUT
    // ========================================================

    if (gTetrisWin.IsVisible &&
        !gTetrisWin.IsMinimized &&
        gWinOrder[TOTAL_WINDOWS-1] == &gTetrisWin) {

        if (Key.ScanCode == SCAN_LEFT) {
            TetrisMove(-1);
            return;
        }

        if (Key.ScanCode == SCAN_RIGHT) {
            TetrisMove(1);
            return;
        }

        if (Key.ScanCode == SCAN_UP) {
            TetrisRotate();
            return;
        }

        if (Key.ScanCode == SCAN_DOWN) {

            if (!TetrisCollision(
                gTetrisX,
                gTetrisY + 1,
                gTetrisPiece,
                gTetrisRotation
            )) {
                gTetrisY++;
            }

            return;
        }

        return;
    }

    // ========================================================
    // NOTES KEYBOARD INPUT
    // ========================================================

    if (gNotesWin.IsVisible &&
        !gNotesWin.IsMinimized &&
        gWinOrder[TOTAL_WINDOWS-1] == &gNotesWin) {

        if (Key.UnicodeChar == 0x08) {

            if (gNotesLen > 0) {
                gNotesLen--;
                gNotesBuffer[gNotesLen] = L'\0';
                gNotesDirty = TRUE;
            }

        } else if (
            Key.UnicodeChar == 0x0D ||
            Key.UnicodeChar == 0x0A
        ) {

            if (gNotesLen < NOTES_MAX_TEXT - 1) {
                gNotesBuffer[gNotesLen++] = L'\n';
                gNotesBuffer[gNotesLen] = L'\0';
                gNotesDirty = TRUE;
            }

        } else if (
            Key.UnicodeChar >= 0x20 &&
            Key.UnicodeChar <= 0x7E &&
            gNotesLen < NOTES_MAX_TEXT - 1
        ) {

            gNotesBuffer[gNotesLen++] =
                Key.UnicodeChar;

            gNotesBuffer[gNotesLen] = L'\0';
            gNotesDirty = TRUE;
        }

        return;
    }

    // ========================================================
    // MINI IDE KEYBOARD INPUT
    // ========================================================

    if (gMiniIDEWin.IsVisible &&
        !gMiniIDEWin.IsMinimized &&
        gWinOrder[TOTAL_WINDOWS-1] == &gMiniIDEWin) {

        if (Key.UnicodeChar == 0x08) {

            if (gIDELen > 0) {
                gIDELen--;
                gIDEBuffer[gIDELen] = L'\0';

                gIDECursor = gIDELen;
                gIDEDirty = TRUE;
            }

        } else if (
            Key.UnicodeChar == 0x0D ||
            Key.UnicodeChar == 0x0A
        ) {

            if (gIDELen < IDE_MAX_TEXT - 1) {

                gIDEBuffer[gIDELen++] = L'\n';
                gIDEBuffer[gIDELen] = L'\0';

                gIDECursor = gIDELen;
                gIDEDirty = TRUE;
            }

        } else if (
            Key.UnicodeChar >= 0x20 &&
            Key.UnicodeChar <= 0x7E &&
            gIDELen < IDE_MAX_TEXT - 1
        ) {

            gIDEBuffer[gIDELen++] =
                Key.UnicodeChar;

            gIDEBuffer[gIDELen] = L'\0';

            gIDECursor = gIDELen;
            gIDEDirty = TRUE;
        }

        return;
    }
    
    // ========================================================
    // UNIT CONVERTER KEYBOARD INPUT
    // ========================================================

    if (
        gUnitConverterWin.IsVisible &&
        !gUnitConverterWin.IsMinimized &&
        gWinOrder[TOTAL_WINDOWS-1] ==
            &gUnitConverterWin
    ) {

        if (Key.UnicodeChar == 0x08) {

            if (gUnitInputLen > 0) {
                gUnitInputLen--;

                gUnitInput[
                    gUnitInputLen
                ] = L'\0';
            }

        } else if (
            Key.UnicodeChar >= L'0' &&
            Key.UnicodeChar <= L'9' &&
            gUnitInputLen < 31
        ) {

            gUnitInput[
                gUnitInputLen++
            ] = Key.UnicodeChar;

            gUnitInput[
                gUnitInputLen
            ] = L'\0';
        }

        return;
    }
    
    // ========================================================
    // SEARCH KEYBOARD INPUT
    // ========================================================

    if (
        gSearchWin.IsVisible &&
        !gSearchWin.IsMinimized &&
        gWinOrder[TOTAL_WINDOWS-1] ==
            &gSearchWin
    ) {

        if (Key.UnicodeChar == 0x08) {

            if (gSearchQueryLen > 0) {
                gSearchQueryLen--;

                gSearchQuery[
                    gSearchQueryLen
                ] = L'\0';

                gSearchDone = FALSE;
            }

        } else if (
            Key.UnicodeChar >= 0x20 &&
            Key.UnicodeChar <= 0x7E &&
            gSearchQueryLen < 63
        ) {

            gSearchQuery[
                gSearchQueryLen++
            ] = Key.UnicodeChar;

            gSearchQuery[
                gSearchQueryLen
            ] = L'\0';

            gSearchDone = FALSE;
        }

        return;
    }
    
    

    if (gPaintTypingText) {
        if (Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A) {
            for (UINTN i=0;i<gPaintTextLen;i++) {
                INT32 px=gPaintTextTargetX+(INT32)i*8;
                INT32 py=gPaintTextTargetY;

                for (INT32 r=0;r<8;r++) {
                    UINT8 row=FONT_8x8[gPaintTextBuf[i]>127?'?':gPaintTextBuf[i]][r];

                    for (INT32 c=0;c<8;c++)
                        if (row&(1<<(7-c))) {
                            INT32 tx=px+c,ty=py+r;

                            if (tx>=0&&tx<PAINT_W&&ty>=0&&ty<PAINT_H)
                                gPaintCanvas[ty*PAINT_W+tx]=gCurrentColor;
                        }
                }
            }

            gPaintTypingText=FALSE;
            gPaintTextBuf[0]=L'\0';
            gPaintTextLen=0;
        } else if (Key.UnicodeChar==0x08&&gPaintTextLen>0) {
            gPaintTextBuf[--gPaintTextLen]=L'\0';
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gPaintTextLen<31) {
            gPaintTextBuf[gPaintTextLen++]=Key.UnicodeChar;
            gPaintTextBuf[gPaintTextLen]=L'\0';
        }
        return;
    }

    if (gTextTermWin.IsVisible&&gInTextTermEditor) {
        if (Key.ScanCode==SCAN_ESC) {
            SaveTextTermFile();
            gInTextTermEditor=FALSE;
        } else if (Key.UnicodeChar==0x08&&gTextTermLen>0) {
            gTextTermBuffer[--gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"Editing... [at save time: Unsaved]");
        } else if ((Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A)&&gTextTermLen<MAX_TEXT-1) {
            gTextTermBuffer[gTextTermLen++]=L'\n';
            gTextTermBuffer[gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"Editing... [at save time: Unsaved]");
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gTextTermLen<MAX_TEXT-1) {
            gTextTermBuffer[gTextTermLen++]=Key.UnicodeChar;
            gTextTermBuffer[gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"Editing... [at save time: Unsaved]");
        }
        return;
    }

    if (gInShellMode||(gTextTermWin.IsVisible&&!gInTextTermEditor)) {
        if (Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A) {
            if (gInShellMode) ExecuteShellCommand();
            else ExecuteCommandString(gShellCmdBuf,TRUE);

            gShellCmdBuf[0]=L'\0';
            gShellCmdLen=0;
        } else if (Key.UnicodeChar==0x08&&gShellCmdLen>0) {
            gShellCmdBuf[--gShellCmdLen]=L'\0';
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gShellCmdLen<127) {
            gShellCmdBuf[gShellCmdLen++]=Key.UnicodeChar;
            gShellCmdBuf[gShellCmdLen]=L'\0';
        }
        return;
    }

    if (gTermWin.IsVisible&&gWinOrder[TOTAL_WINDOWS-1]==&gTermWin) {
        if (Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A)
            ExecuteTermCommand();
        else if (Key.UnicodeChar==0x08&&gTermCmdLen>0)
            gTermCmdBuf[--gTermCmdLen]=L'\0';
        else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gTermCmdLen<127) {
            gTermCmdBuf[gTermCmdLen++]=Key.UnicodeChar;
            gTermCmdBuf[gTermCmdLen]=L'\0';
        }
        return;
    }

    if (gSnakeWin.IsVisible) {
        if (Key.ScanCode==SCAN_UP&&gSnakeDirY!=1) {gSnakeDirX=0;gSnakeDirY=-1;}
        if (Key.ScanCode==SCAN_DOWN&&gSnakeDirY!=-1) {gSnakeDirX=0;gSnakeDirY=1;}
        if (Key.ScanCode==SCAN_LEFT&&gSnakeDirX!=1) {gSnakeDirX=-1;gSnakeDirY=0;}
        if (Key.ScanCode==SCAN_RIGHT&&gSnakeDirX!=-1) {gSnakeDirX=1;gSnakeDirY=0;}
    }

    if (gFindWin.IsVisible) {
        CHAR16 *TargetBuf=(gActiveField==0)?gFindBuf:gReplaceBuf;
        UINTN *TargetLen=(gActiveField==0)?&gFindLen:&gReplaceLen;

        if (Key.UnicodeChar==0x08&&*TargetLen>0)
            TargetBuf[--(*TargetLen)]=L'\0';
        else if (Key.UnicodeChar==0x09)
            gActiveField=!gActiveField;
        else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&*TargetLen<31) {
            TargetBuf[(*TargetLen)++]=Key.UnicodeChar;
            TargetBuf[*TargetLen]=L'\0';
        }

        return;
    }

    if (gEditorWin.IsVisible) {
        if (Key.UnicodeChar==0x08&&gTextLen>0) {
            gTextBuffer[--gTextLen]=L'\0';
        } else if (Key.UnicodeChar==0x09) {
            for (INT32 k=0;k<4&&gTextLen<MAX_TEXT-1;k++) gTextBuffer[gTextLen++] = L' ';
            gTextBuffer[gTextLen]=L'\0';
        } else if ((Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A)&&gTextLen<MAX_TEXT-1) {
            gTextBuffer[gTextLen++]=L'\n';
            gTextBuffer[gTextLen]=L'\0';
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gTextLen<MAX_TEXT-1) {
            gTextBuffer[gTextLen++]=Key.UnicodeChar;
            gTextBuffer[gTextLen]=L'\0';
        }
    }
}

// ============================================================
// TASKBAR + START MENU + POWER DIALOG
// ============================================================

VOID RenderTaskbarAndStartMenu(VOID) {

    INT32 TaskbarY =
        (INT32)gScreenHeight - TASKBAR_H;

    // ========================================================
    // TASKBAR BACKGROUND
    // ========================================================

    DrawRect(
        0,
        TaskbarY,
        gScreenWidth,
        TASKBAR_H,
        COLOR_TOPBAR_BG
    );

    // ========================================================
    // START BUTTON
    // ========================================================

    DrawButton(
        4,
        TaskbarY + 4,
        TASKBAR_START_W,
        TASKBAR_BUTTON_H,
        L"Start",
        gStartMenuOpen
            ? COLOR_TITLE_ACTIVE
            : COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    // ========================================================
    // SCROLL BUTTONS
    // ========================================================

    INT32 LeftArrowX =
        TASKBAR_START_W + 8;

    INT32 WindowAreaX =
        LeftArrowX + TASKBAR_SCROLL_W + 4;

    INT32 RightArrowX =
        WindowAreaX +
        (INT32)GetTaskbarVisibleCount() *
        TASKBAR_BUTTON_W +
        4;

    DrawButton(
        LeftArrowX,
        TaskbarY + 4,
        TASKBAR_SCROLL_W,
        TASKBAR_BUTTON_H,
        L"<",
        COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    DrawButton(
        RightArrowX,
        TaskbarY + 4,
        TASKBAR_SCROLL_W,
        TASKBAR_BUTTON_H,
        L">",
        COLOR_TOOLBAR_BG,
        COLOR_TEXT
    );

    // ========================================================
    // TASKBAR WINDOW BUTTONS
    // ========================================================

    UINTN VisibleCount =
        GetTaskbarVisibleCount();

    UINTN VisibleDrawn = 0;

    for (
        UINTN i = gTaskbarScroll;
        i < TASKBAR_WINDOW_COUNT;
        i++
    ) {

        UEFI_WINDOW *Win =
            gTaskbarWindows[i];

        if (!Win)
            continue;

        if (!Win->IsVisible)
            continue;

        if (VisibleDrawn >= VisibleCount)
            break;

        INT32 ButtonX =
            WindowAreaX +
            (INT32)VisibleDrawn *
            TASKBAR_BUTTON_W;

        BOOLEAN IsActive =
            !Win->IsMinimized &&
            gWinOrder[TOTAL_WINDOWS - 1] ==
                Win;

        DrawButton(
            ButtonX,
            TaskbarY + 4,
            TASKBAR_BUTTON_W - 4,
            TASKBAR_BUTTON_H,
            gTaskbarNames[i],
            IsActive
                ? COLOR_TITLE_ACTIVE
                : COLOR_TOOLBAR_BG,
            COLOR_TEXT
        );

        VisibleDrawn++;
    }

    // ========================================================
    // CLOCK
    // ========================================================

    INT32 ClockX =
        (INT32)gScreenWidth -
        TASKBAR_POWER_W -
        TASKBAR_CLOCK_W -
        8;

    EFI_TIME Time;

    CHAR16 TimeBuf[16] =
        L"--:--:--";

    if (!EFI_ERROR(
        gRT->GetTime(
            &Time,
            NULL
        )
    )) {

        UnicodeSPrint(
            TimeBuf,
            sizeof(TimeBuf),
            L"%02d:%02d:%02d",
            Time.Hour,
            Time.Minute,
            Time.Second
        );
    }

    DrawRect(
        ClockX,
        TaskbarY + 4,
        TASKBAR_CLOCK_W,
        TASKBAR_BUTTON_H,
        COLOR_TOOLBAR_BG
    );

    DrawString(
        TimeBuf,
        ClockX + 6,
        TaskbarY + 12,
        COLOR_TEXT
    );

    // ========================================================
    // POWER
    // ========================================================

    DrawButton(
        (INT32)gScreenWidth -
        TASKBAR_POWER_W -
        4,

        TaskbarY + 4,

        TASKBAR_POWER_W,
        TASKBAR_BUTTON_H,

        L"Power",

        COLOR_BTN_CLOSE,
        COLOR_TEXT
    );

    // ========================================================
    // START MENU
    // ========================================================

    if (gStartMenuOpen) {

        INT32 MenuW = 430;
        INT32 MenuH = 300;

        INT32 MenuX = 6;

        INT32 MenuY =
            TaskbarY -
            MenuH -
            4;

        DrawRect(
            MenuX,
            MenuY,
            MenuW,
            MenuH,
            COLOR_WIN_BG
        );

        DrawRect(
            MenuX,
            MenuY,
            MenuW,
            22,
            COLOR_TITLE_ACTIVE
        );

        DrawString(
            L"Applications",
            MenuX + 10,
            MenuY + 7,
            COLOR_TEXT
        );

        CONST CHAR16 *MenuItems[] = {

            L"Text Editor",
            L"Paint Studio",
            L"Terminal",
            L"File Manager",

            L"Animation Editor",
            L"Animation Viewer",

            L"Calculator",
            L"System Info",
            L"Snake Game",

            L"TextTerm Shell",
            L"Disk Utility",
            L"Wallpaper",
            L"RoboChat",

            L"Task Manager",
            L"Notes",
            L"Mini IDE",
            L"Tetris",
            L"Common Apps",

            L"Image Viewer",
            L"File Archive",
            L"Clipboard",
            L"Unit Converter",
            L"Search",
            L"PC Speaker"
        };

        UINTN MenuCount =
            sizeof(MenuItems) /
            sizeof(MenuItems[0]);

        for (
            UINTN i = 0;
            i < MenuCount;
            i++
        ) {

            INT32 Column =
                (INT32)(i / 13);

            INT32 Row =
                (INT32)(i % 13);

            INT32 BX =
                MenuX +
                6 +
                Column * 210;

            INT32 BY =
                MenuY +
                26 +
                Row * 20;

            DrawButton(
                BX,
                BY,
                198,
                18,
                MenuItems[i],
                COLOR_TOOLBAR_BG,
                COLOR_TEXT
            );
        }
    }

    // ========================================================
    // POWER DIALOG
    // ========================================================

    if (gShowPowerDialog) {

        INT32 DiaW = 320;
        INT32 DiaH = 160;

        INT32 DiaX =
            ((INT32)gScreenWidth -
             DiaW) / 2;

        INT32 DiaY =
            ((INT32)gScreenHeight -
             DiaH) / 2;

        DrawRect(
            DiaX,
            DiaY,
            DiaW,
            DiaH,
            COLOR_WIN_BG
        );

        DrawRect(
            DiaX,
            DiaY,
            DiaW,
            24,
            COLOR_TITLE_ACTIVE
        );

        DrawString(
            L"Power popup",
            DiaX + 10,
            DiaY + 6,
            COLOR_TEXT
        );

        DrawString(
            L"Select a power operation:",
            DiaX + 15,
            DiaY + 34,
            COLOR_TEXT
        );

        DrawButton(
            DiaX + 20,
            DiaY + 55,
            80,
            30,
            L"Poweroff",
            COLOR_BTN_CLOSE,
            COLOR_TEXT
        );

        DrawButton(
            DiaX + 120,
            DiaY + 55,
            80,
            30,
            L"Reboot",
            COLOR_TOOLBAR_BG,
            COLOR_TEXT
        );

        DrawButton(
            DiaX + 220,
            DiaY + 55,
            80,
            30,
            L"Shell",
            NEW_APP_COLOR_GOOD,
            COLOR_TEXT
        );

        DrawButton(
            DiaX + 110,
            DiaY + 105,
            100,
            25,
            L"Cancel",
            COLOR_WIN_BORDER,
            COLOR_TEXT
        );
    }
}

VOID DrawMouseCursor(INT32 x,INT32 y) {
    for (INT32 r=0;r<16;r++)
        for (INT32 c=0;c<16;c++) {
            UINT8 pixel=MOUSE_CURSOR_16x16[r][c];

            if (pixel==1) FastDrawPixel(x+c,y+r,RGB(0,0,0));
            else if (pixel==2) FastDrawPixel(x+c,y+r,RGB(255,255,255));
        }
}

VOID ProcessInput(BOOLEAN Click,BOOLEAN LastClick) {
    if (!Click) {
        for (INT32 i=0;i<TOTAL_WINDOWS;i++)
            if (gWinOrder[i]) gWinOrder[i]->IsDragging=FALSE;
    } else {
        for (INT32 i=0;i<TOTAL_WINDOWS;i++)
            if (gWinOrder[i]&&gWinOrder[i]->IsDragging) {
                gWinOrder[i]->X=gMouseX-gWinOrder[i]->DragOffsetX;
                gWinOrder[i]->Y=gMouseY-gWinOrder[i]->DragOffsetY;
            }
    }

    if (!Click||LastClick) return;

    INT32 TaskbarY =
        (INT32)gScreenHeight - TASKBAR_H;

    // Power Dialog
    if (gShowPowerDialog) {
        INT32 DiaW=320,DiaH=160;
        INT32 DiaX=((INT32)gScreenWidth-DiaW)/2;
        INT32 DiaY=((INT32)gScreenHeight-DiaH)/2;

        if (IsInRect(gMouseX,gMouseY,DiaX+20,DiaY+55,80,30)) {
            ShowShutdownSequence(FALSE);
        } else if (IsInRect(gMouseX,gMouseY,DiaX+120,DiaY+55,80,30)) {
            ShowShutdownSequence(TRUE);
        } else if (IsInRect(gMouseX,gMouseY,DiaX+220,DiaY+55,80,30)) {
            gInShellMode=TRUE;
            gShowPowerDialog=FALSE;
            gStartMenuOpen=FALSE;
        } else if (IsInRect(gMouseX,gMouseY,DiaX+110,DiaY+105,100,25)) {
            gShowPowerDialog=FALSE;
        }

        return;
    }

if (gStartMenuOpen) {

    INT32 MenuW = 430;
    INT32 MenuH = 300;
    INT32 MenuX = 6;
    INT32 MenuY =
        TaskbarY - MenuH - 4;

    if (IsInRect(
        gMouseX,
        gMouseY,
        MenuX,
        MenuY,
        MenuW,
        MenuH
    )) {

        UEFI_WINDOW *AppWins[] = {
            &gEditorWin,
            &gPaintWin,
            &gTermWin,
            &gFileWin,
            &gAnimEditWin,
            &gAnimViewWin,
            &gCalcWin,
            &gSysInfoWin,
            &gSnakeWin,
            &gTextTermWin,
            &gDiskUtilWin,
            &gWallpaperWin,
            &gRoboChatWin,
            &gTaskMgrWin,

            &gNotesWin,
            &gMiniIDEWin,
            &gTetrisWin,
            &gCommonAppsWin,
            &gImageViewerWin,
            &gArchiveWin,
            &gClipboardWin,
            &gUnitConverterWin,
            &gSearchWin,
            &gMusicWin
        };

        UINTN AppCount =
            sizeof(AppWins) /
            sizeof(AppWins[0]);

        for (
            UINTN i = 0;
            i < AppCount;
            i++
        ) {
            INT32 Column =
                (INT32)(i / 13);

            INT32 Row =
                (INT32)(i % 13);

            INT32 BX =
                MenuX + 6 +
                Column * 210;

            INT32 BY =
                MenuY + 26 +
                Row * 20;

            if (IsInRect(
                gMouseX,
                gMouseY,
                BX,
                BY,
                198,
                18
            )) {

                AppWins[i]->IsVisible =
                    TRUE;

                AppWins[i]->IsMinimized =
                    FALSE;

                BringToFront(
                    AppWins[i]
                );

                gStartMenuOpen =
                    FALSE;

                return;
            }
        }

    } else if (!IsInRect(
        gMouseX,
        gMouseY,
        6,
        TaskbarY + 4,
        60,
        24
    )) {

        gStartMenuOpen = FALSE;
    }
}

        // ========================================================
    // OS-STYLE TASKBAR INPUT
    // ========================================================

    if (IsInRect(
        gMouseX,
        gMouseY,
        0,
        TaskbarY,
        gScreenWidth,
        TASKBAR_H
    )) {

        // ----------------------------------------------------
        // START BUTTON
        // ----------------------------------------------------

        if (IsInRect(
            gMouseX,
            gMouseY,
            4,
            TaskbarY + 4,
            TASKBAR_START_W,
            TASKBAR_BUTTON_H
        )) {

            gStartMenuOpen =
                !gStartMenuOpen;

            return;
        }

        // ----------------------------------------------------
        // LEFT SCROLL
        // ----------------------------------------------------

        INT32 LeftArrowX =
            TASKBAR_START_W + 8;

        INT32 WindowAreaX =
            LeftArrowX +
            TASKBAR_SCROLL_W +
            4;

        if (IsInRect(
            gMouseX,
            gMouseY,
            LeftArrowX,
            TaskbarY + 4,
            TASKBAR_SCROLL_W,
            TASKBAR_BUTTON_H
        )) {

            if (gTaskbarScroll > 0)
                gTaskbarScroll--;

            return;
        }

        // ----------------------------------------------------
        // VISIBLE APP BUTTONS
        // ----------------------------------------------------

        UINTN VisibleCount =
            GetTaskbarVisibleCount();

        UINTN VisibleIndex = 0;

        for (
            UINTN i = gTaskbarScroll;
            i < TASKBAR_WINDOW_COUNT;
            i++
        ) {

            UEFI_WINDOW *Win =
                gTaskbarWindows[i];

            if (!Win)
                continue;

            if (!Win->IsVisible)
                continue;

            if (VisibleIndex >= VisibleCount)
                break;

            INT32 ButtonX =
                WindowAreaX +
                (INT32)VisibleIndex *
                TASKBAR_BUTTON_W;

            if (IsInRect(
                gMouseX,
                gMouseY,
                ButtonX,
                TaskbarY + 4,
                TASKBAR_BUTTON_W - 4,
                TASKBAR_BUTTON_H
            )) {

                // Minimized -> restore
                if (Win->IsMinimized) {

                    Win->IsMinimized =
                        FALSE;

                    BringToFront(Win);

                }

                // Active -> minimize
                else if (
                    gWinOrder[TOTAL_WINDOWS - 1]
                    == Win
                ) {

                    Win->IsMinimized =
                        TRUE;

                }

                // Inactive -> activate
                else {

                    BringToFront(Win);
                }

                return;
            }

            VisibleIndex++;
        }

        // ----------------------------------------------------
        // RIGHT SCROLL
        // ----------------------------------------------------

        INT32 RightArrowX =
            WindowAreaX +
            (INT32)VisibleCount *
            TASKBAR_BUTTON_W +
            4;

        if (IsInRect(
            gMouseX,
            gMouseY,
            RightArrowX,
            TaskbarY + 4,
            TASKBAR_SCROLL_W,
            TASKBAR_BUTTON_H
        )) {

            if (
                gTaskbarScroll +
                VisibleCount <
                TASKBAR_WINDOW_COUNT
            ) {

                gTaskbarScroll++;
            }

            return;
        }

        // ----------------------------------------------------
        // POWER
        // ----------------------------------------------------

        INT32 PowerX =
            (INT32)gScreenWidth -
            TASKBAR_POWER_W -
            4;

        if (IsInRect(
            gMouseX,
            gMouseY,
            PowerX,
            TaskbarY + 4,
            TASKBAR_POWER_W,
            TASKBAR_BUTTON_H
        )) {

            gShowPowerDialog =
                TRUE;

            return;
        }

        return;
    }

    // Window manager
    for (INT32 i=TOTAL_WINDOWS-1;i>=0;i--) {
        UEFI_WINDOW *Win=gWinOrder[i];

        if (!Win||!Win->IsVisible||Win->IsMinimized) continue;

        if (IsInRect(gMouseX,gMouseY,Win->X,Win->Y,Win->W,26)) {
            BringToFront(Win);

            INT32 BtnW=24;
            INT32 CloseX=Win->X+Win->W-BtnW;
            INT32 MaxX=CloseX-BtnW;
            INT32 MinX=MaxX-BtnW;

            if (IsInRect(gMouseX,gMouseY,CloseX,Win->Y,BtnW,26)) {
                Win->IsVisible=FALSE;
            } else if (IsInRect(gMouseX,gMouseY,MaxX,Win->Y,BtnW,26)) {
                ToggleWindowMaximize(Win);
            } else if (IsInRect(gMouseX,gMouseY,MinX,Win->Y,BtnW,26)) {
                Win->IsMinimized=TRUE;
            } else {
                Win->IsDragging=TRUE;
                Win->DragOffsetX=gMouseX-Win->X;
                Win->DragOffsetY=gMouseY-Win->Y;
            }

            return;
        }

        if (IsInRect(gMouseX,gMouseY,Win->X,Win->Y+26,Win->W,Win->H-26)) {
            BringToFront(Win);

            if (Win->Input)
                Win->Input(Win,gMouseX,gMouseY,Click,LastClick);

            return;
        }
    }
}

// --- Entry Point ---
EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status=gBS->LocateProtocol(
        &gEfiGraphicsOutputProtocolGuid,
        NULL,
        (VOID**)&gGop
    );

    if (EFI_ERROR(Status)) return Status;

    gScreenWidth=gGop->Mode->Info->HorizontalResolution;
    gScreenHeight=gGop->Mode->Info->VerticalResolution;
    gPixelsPerScanLine=gGop->Mode->Info->PixelsPerScanLine;
    gFrameBuffer=(UINT32*)(UINTN)gGop->Mode->FrameBufferBase;

    UINTN FrameBufferSize=gPixelsPerScanLine*gScreenHeight*sizeof(UINT32);

    Status=gBS->AllocatePool(EfiLoaderData,FrameBufferSize,(VOID**)&gBackBuffer);
    if (EFI_ERROR(Status)) gBackBuffer=gFrameBuffer;

    EFI_HANDLE *PointerHandles=NULL;
    UINTN NumHandles=0;

    Status=gBS->LocateHandleBuffer(
        ByProtocol,
        &gEfiSimplePointerProtocolGuid,
        NULL,
        &NumHandles,
        &PointerHandles
    );

    if (!EFI_ERROR(Status)) {
        for (UINTN i=0;i<NumHandles&&gNumMouseDevices<MAX_POINTER_DEVICES;i++) {
            EFI_SIMPLE_POINTER_PROTOCOL *Ptr=NULL;

            if (!EFI_ERROR(gBS->HandleProtocol(
                PointerHandles[i],
                &gEfiSimplePointerProtocolGuid,
                (VOID**)&Ptr))) {

                gMouseDevices[gNumMouseDevices++]=Ptr;
                Ptr->Reset(Ptr,TRUE);
            }
        }

        gBS->FreePool(PointerHandles);
    }

    gBS->LocateProtocol(&gEfiAbsolutePointerProtocolGuid,NULL,(VOID**)&gAbsMouse);
    if (gAbsMouse) gAbsMouse->Reset(gAbsMouse,TRUE);

    ResetSnakeGame();
    ResetTetris();
    RefreshFileList();

    // RESTORED STARTUP SCREEN + JINGLE
    ShowStartupScreen();

    BOOLEAN LastLeftClick=FALSE;

    while (TRUE) {
        BOOLEAN LeftClick=FALSE;

        PollNewAppsKeyboard();
        PollKeyboard();
        PollMouse(&LeftClick);

        if (
            gJumpscareActive &&
            gJumpscareSoundPending
        ) {
            gJumpscareSoundPending = FALSE;
            PlayJumpscareBeep();
        }

        if (gJumpscareActive || gJumpscareMessage) {
            UpdateJumpscare();
        }

        if (gJumpscareActive) {
            RenderJumpscare();
        }

        else if (gJumpscareMessage) {
            RenderJumpscareMessage();
        }

        else if (gInShellMode) {
            RenderShellMode();
        }

        else {
            RenderNewWallpaper();

            if (gSnakeWin.IsVisible&&!gSnakeWin.IsMinimized) {
                gSnakeTick++;

                if (gSnakeTick%5==0)
                    UpdateSnakeGame();
            }
            
            if (gTetrisWin.IsVisible &&
                !gTetrisWin.IsMinimized &&
                !gTetrisGameOver) {

                EFI_TIME TetrisTime;

                if (!EFI_ERROR(
                    gRT->GetTime(&TetrisTime, NULL)
                )) {

                    if (TetrisTime.Second !=
                        gTetrisLastSecond) {

                        gTetrisLastSecond =
                            TetrisTime.Second;

                        UpdateTetris();
                    }
                }
            }

            if (gStopwatchRunning) {

                EFI_TIME StopwatchTime;

                if (!EFI_ERROR(
                    gRT->GetTime(
                        &StopwatchTime,
                        NULL
                    )
                )) {

                    if (
                        StopwatchTime.Second !=
                        gStopwatchLastSecond
                    ) {

                        gStopwatchLastSecond =
                            StopwatchTime.Second;

                        gStopwatchTicks++;

                        if (gStopwatchTicks > 359999)
                            gStopwatchTicks = 0;
                    }
                }
            }

            if (gAnimPlaying) {
                gAnimTickCount++;

                if (gAnimTickCount>=gAnimSpeedDelay) {
                    gAnimTickCount=0;
                    gViewFrameIdx=(gViewFrameIdx+1)%gAnimFrameCount;
                }
            }

            for (INT32 i=0;i<TOTAL_WINDOWS;i++) {
                UEFI_WINDOW *Win=gWinOrder[i];

                if (Win&&Win->IsVisible&&!Win->IsMinimized)
                    Win->Render(Win);
            }

            ProcessInput(LeftClick,LastLeftClick);

            if (gWallpaperAnimate)
                gWallpaperPhase++;

            RenderTaskbarAndStartMenu();
            DrawMouseCursor(gMouseX,gMouseY);
        }

        SwapBuffers();
        LastLeftClick=LeftClick;

        gBS->Stall(10000);
    }

    return EFI_SUCCESS;
} // dk if uefitext1337 works or not? ._.