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
#include <emmintrin.h>

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

#if defined(_MSC_VER)
  #define read_tsc() __rdtsc()
#else
  STATIC inline UINT64 read_tsc(VOID) {
      UINT32 lo, hi;
      __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
      return ((UINT64)hi << 32) | (UINT64)lo;
  }
#endif

#ifndef EFI_ABSPTR_TOUCH_OR_FIRST_BUTTON
#define EFI_ABSPTR_TOUCH_OR_FIRST_BUTTON 0x00000001
#endif
#ifndef EFI_ABSP_TouchActive
#define EFI_ABSP_TouchActive 0x00000001
#endif

#define RGB(r,g,b)          ((UINT32)(((r)<<16)|((g)<<8)|(b)))

/* ============================================================
 * RUNTIME-THEMABLE COLORS
 * ============================================================ */
STATIC UINT32 gThemeDesktop     = RGB(30, 41, 59);
STATIC UINT32 gThemeTopbarBg    = RGB(15, 23, 42);
STATIC UINT32 gThemeWinBg       = RGB(51, 65, 85);
STATIC UINT32 gThemeWinBorder   = RGB(100, 116, 139);
STATIC UINT32 gThemeCanvas      = RGB(15, 15, 15);
STATIC UINT32 gThemeTitleActive = RGB(37, 99, 235);
STATIC UINT32 gThemeBtnClose    = RGB(225, 29, 72);
STATIC UINT32 gThemeText        = RGB(241, 245, 249);
STATIC UINT32 gThemeCursor      = RGB(59, 130, 246);
STATIC UINT32 gThemeToolbarBg   = RGB(30, 58, 138);
STATIC UINT32 gThemeMenuBg      = RGB(30, 41, 59);
STATIC UINT32 gThemeMinimapBg   = RGB(20, 25, 35);

#define COLOR_DESKTOP      gThemeDesktop
#define COLOR_TOPBAR_BG    gThemeTopbarBg
#define COLOR_WIN_BG       gThemeWinBg
#define COLOR_WIN_BORDER   gThemeWinBorder
#define COLOR_CANVAS       gThemeCanvas
#define COLOR_TITLE_ACTIVE gThemeTitleActive
#define COLOR_BTN_CLOSE    gThemeBtnClose
#define COLOR_TEXT         gThemeText
#define COLOR_CURSOR       gThemeCursor
#define COLOR_TOOLBAR_BG   gThemeToolbarBg
#define COLOR_MENU_BG      gThemeMenuBg
#define COLOR_MINIMAP_BG   gThemeMinimapBg

STATIC UINT32  gFrameDelayUs = 0;
STATIC UINT64 gTscPerUs = 0;
STATIC UINT32 gFps = 0;
STATIC UINT32 gFrameCounter = 0;
STATIC UINT64 gFpsWindowStart = 0;
STATIC BOOLEAN gSoundEnabled = FALSE;

#define MAX_WINDOWS 64
STATIC UINTN gWindowCount = 0;

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

#define TASKBAR_H            32
#define TASKBAR_START_W      62
#define TASKBAR_SCROLL_W     24
#define TASKBAR_BUTTON_W     62
#define TASKBAR_BUTTON_H     24
#define TASKBAR_CLOCK_W      120
#define TASKBAR_POWER_W      64
#define TASKBAR_GAP          4

#define SNAKE_GRID_W        20
#define SNAKE_GRID_H        15
#define SNAKE_CELL_SZ       12
#define SNAKE_MAX_LEN       (SNAKE_GRID_W * SNAKE_GRID_H)

#define STORE_APP_COUNT 5
#define BROWSER_URL_MAX 128
#define OFFICE_DOC_MAX 4096
#define OFFICE_SHEET_MAX 2048
#define OFFICE_SLIDE_MAX 2048

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
STATIC CHAR16 gTextFileName[64] = L"untitled.txt";
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
#define SANDBOX_GRID_W 180
#define SANDBOX_GRID_H 100
#define SANDBOX_CELL_SIZE 3

typedef enum {
    SB_EMPTY = 0,
    SB_SAND,
    SB_WATER,
    SB_WALL,
    SB_MUD,
    SB_STONE,
    SB_FIRE,
    SB_STEAM,
    SB_SMOKE,
    SB_OIL,
    SB_LAVA,
    SB_ACID,
    SB_ICE,
    SB_PLANT
} SANDBOX_MATERIAL;

STATIC UINT8 gSandboxCells[SANDBOX_GRID_W * SANDBOX_GRID_H];
STATIC UINT8 gSandboxTool = SB_SAND;
STATIC BOOLEAN gSandboxPaused = FALSE;
STATIC UINT32 gSandboxRng = 0x73A91C5D;
STATIC UINT32 gSandboxTick = 0;

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
#define GUI_MATRIX_MAX_COLUMNS 256

STATIC BOOLEAN gGuiMatrixActive = FALSE;
STATIC INT32 gGuiMatrixDrops[GUI_MATRIX_MAX_COLUMNS];
STATIC UINT32 gGuiMatrixSeed = 0xA5C31E27U;

/* Scrollable command and TexLag documentation viewer. */
STATIC UINTN gHelpScroll = 0;
STATIC BOOLEAN gShellHelpActive = FALSE;

#define SHELL_HELP_LINES_PER_PAGE 12

STATIC CONST CHAR16 *gHelpLines[] = {
    L"UEFITEXT HELP",
    L"Use Top, Up, Down and Bottom to scroll.",
    L"Text Shell: SPACE next, BACKSPACE previous, ESC exit.",
    L"",
    L"[DESKTOP]",
    L"Open apps from the Start menu or taskbar.",
    L"Drag window title bars to move windows.",
    L"Use the title-bar buttons to minimize or maximize.",
    L"",
    L"[TEXT EDITOR]",
    L"Open the Text Editor to read or write text files.",
    L"File > Open loads an existing file.",
    L"File > Save As saves text with a chosen filename.",
    L"Edit > Find & Replace searches and changes text.",
    L"View controls word wrap and the minimap.",
    L"Syntax highlighting and encoding support are improvements",
    L"being added to the regular Text Editor.",
    L"",
    L"[FILES]",
    L"File Manager lists files on UEFI-accessible volumes.",
    L"UEFIText cannot browse Linux folders like /home directly.",
    L"Use a filesystem that is accessible through UEFI.",
    L"",
    L"[MUSIC]",
    L"Open PC Speaker Music to hear computer-generated tones.",
    L"The PC speaker uses the PIT timer.",
    L"",
    L"[GAMES AND UTILITIES]",
    L"Open apps from the Start menu or taskbar.",
    L"Use Settings to change supported desktop options.",
    L"",
    L"[TERMINAL COMMANDS]",
    L"help       Open this guide.",
    L"gui        Return to the desktop.",
    L"ls         List accessible files.",
    L"clear      Clear terminal output.",
    L"version    Show the version.",
    L"reboot     Restart the computer.",
    L"poweroff   Request shutdown.",
    L"",
    L"[IMPORTANT]",
    L"UEFIText is a UEFI application, not a full Linux OS.",
    L"Only use commands and features that are implemented.",
    L"TexLag was made but gone due to sanity reasons I WAS AT IN SANITY MODE SO BYE BYE TEXLAG I GUESS!!!!",
    L"",
    L"ends here i guess."
};

#define HELP_LINE_COUNT \
    (sizeof(gHelpLines) / sizeof(gHelpLines[0]))

#define NEW_APP_COLOR_BG       RGB(24,31,45)
#define NEW_APP_COLOR_PANEL    RGB(30,41,59)
#define NEW_APP_COLOR_ACCENT   RGB(59,130,246)
#define NEW_APP_COLOR_GOOD     RGB(34,197,94)
#define NEW_APP_COLOR_WARN     RGB(234,179,8)
#define NEW_APP_COLOR_BAD      RGB(225,29,72)

#define NOTES_MAX_TEXT 8192
STATIC CHAR16 gNotesBuffer[NOTES_MAX_TEXT];
STATIC UINTN gNotesLen = 0;
STATIC CHAR16 gNotesFileName[64] = L"Notes.txt";
STATIC BOOLEAN gNotesDirty = FALSE;

#define IDE_MAX_TEXT 12288
STATIC CHAR16 gIDEBuffer[IDE_MAX_TEXT];
STATIC UINTN gIDELen = 0;
#define TL_MAX_DRAW_COMMANDS 64
#define TL_DRAW_TEXT_CAPACITY 128

typedef enum {
    TL_DRAW_TEXT,
    TL_DRAW_RECT,
    TL_DRAW_CIRCLE
} TL_DRAW_KIND;

typedef struct {
    TL_DRAW_KIND Kind;
    INT32 X, Y, W, H;
    UINT32 Color;
    CHAR16 Text[TL_DRAW_TEXT_CAPACITY];
} TL_DRAW_COMMAND;

STATIC TL_DRAW_COMMAND gTLDrawCommands[TL_MAX_DRAW_COMMANDS];
STATIC UINTN gTLDrawCommandCount = 0;
STATIC UINT32 gTLWindowBackground = RGB(24,31,45);
STATIC CHAR16 gIDEFileName[64] = L"main.c";
STATIC UINTN gIDECursor = 0;
STATIC BOOLEAN gIDEDirty = FALSE;

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
STATIC UINT8 gTetrisLastSecond = 0;
STATIC BOOLEAN gTetrisGameOver = FALSE;
STATIC UINT32 gTetrisRandomState = 0x13579BDF;

typedef enum {
    COMMON_APP_CALENDAR,
    COMMON_APP_CLOCK,
    COMMON_APP_STOPWATCH,
    COMMON_APP_ABOUT
} COMMON_APP_TAB;
STATIC COMMON_APP_TAB gCommonAppTab = COMMON_APP_CALENDAR;

STATIC BOOLEAN gCalendarInitialized = FALSE;
STATIC UINT16 gCalendarYear = 2026;
STATIC UINT8 gCalendarMonth = 1;
STATIC UINT8 gCalendarSelectedDay = 1;

STATIC BOOLEAN gStopwatchRunning = FALSE;
STATIC UINT32 gStopwatchTicks = 0;
STATIC UINT8 gStopwatchLastSecond = 0;

STATIC UINT32 gWallpaperColor1 = RGB(30,41,59);
STATIC UINT32 gWallpaperColor2 = RGB(15,23,42);
STATIC UINTN gWallpaperMode = 0;
STATIC BOOLEAN gWallpaperAnimate = FALSE;
STATIC UINT32 gWallpaperPhase = 0;

STATIC UINT32 *gWallpaperCache = NULL;
STATIC UINTN   gWallpaperCacheW = 0, gWallpaperCacheH = 0;
STATIC BOOLEAN gWallpaperCacheDirty = TRUE;
STATIC UINT32  gWallpaperRebuilds = 0;

STATIC CHAR16 gRoboInput[96] = L"";
STATIC UINTN gRoboInputLen = 0;
STATIC CHAR16 gRoboLog[10][96];
STATIC UINTN gRoboLogCount = 0;
STATIC INT32 gTaskMgrSelected = -1;

STATIC BOOLEAN gJumpscareActive = FALSE;
STATIC UINT32 gJumpscareElapsedMs = 0;
STATIC UINT32 gJumpscareRandom = 0xA53C91E7;
STATIC BOOLEAN gJumpscareSoundPending = FALSE;
STATIC BOOLEAN gJumpscareMessage = FALSE;
STATIC UINT32 gJumpscareMessageTimer = 0;

STATIC UINTN gTaskbarScroll = 0;
STATIC UINTN gStartMenuScroll = 0;

STATIC INT32 gClipX0 = 0, gClipY0 = 0, gClipX1 = 1024, gClipY1 = 768;

/* Store state */
STATIC INT32 gStoreSelected = -1;

/* Browser state */
STATIC CHAR16 gBrowserUrl[BROWSER_URL_MAX] = L"";
STATIC UINTN gBrowserUrlLen = 0;
STATIC INT32 gBrowserPage = 0;

/* Office state */
STATIC INT32 gOfficeTab = 0;
STATIC CHAR16 gOfficeDocBuf[OFFICE_DOC_MAX] = L"";
STATIC UINTN gOfficeDocLen = 0;
STATIC CHAR16 gOfficeSheetBuf[OFFICE_SHEET_MAX] = L"";
STATIC UINTN gOfficeSheetLen = 0;
STATIC CHAR16 gOfficeSlideBuf[OFFICE_SLIDE_MAX] = L"";
STATIC UINTN gOfficeSlideLen = 0;

#define TILE_SHIFT  6
#define TILE_SIZE   (1 << TILE_SHIFT)
#define MAX_TILES_X ((2048 + TILE_SIZE - 1) / TILE_SIZE)
#define MAX_TILES_Y ((1280 + TILE_SIZE - 1) / TILE_SIZE)
STATIC UINT8 gDirtyBits[(MAX_TILES_X * MAX_TILES_Y + 7) / 8];

STATIC UINT32  gCursorSave[16][16];
STATIC INT32   gCursorSaveX = -1, gCursorSaveY = -1;
STATIC BOOLEAN gDesktopDirty = TRUE;
STATIC BOOLEAN gRedrawWindows = FALSE;

STATIC inline VOID MarkDirtyTile(INT32 tx, INT32 ty) {
    if (tx < 0 || ty < 0 || tx >= MAX_TILES_X || ty >= MAX_TILES_Y) return;
    INT32 bit = ty * MAX_TILES_X + tx;
    gDirtyBits[bit >> 3] |= (UINT8)(1u << (bit & 7));
}

STATIC VOID MarkDirtyRect(INT32 x0, INT32 y0, INT32 x1, INT32 y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (INT32)gScreenWidth)  x1 = (INT32)gScreenWidth;
    if (y1 > (INT32)gScreenHeight) y1 = (INT32)gScreenHeight;
    if (x0 >= x1 || y0 >= y1) return;
    INT32 tx0 = x0 >> TILE_SHIFT;
    INT32 ty0 = y0 >> TILE_SHIFT;
    INT32 tx1 = (x1 - 1) >> TILE_SHIFT;
    INT32 ty1 = (y1 - 1) >> TILE_SHIFT;
    for (INT32 ty = ty0; ty <= ty1; ty++)
        for (INT32 tx = tx0; tx <= tx1; tx++)
            MarkDirtyTile(tx, ty);
}

STATIC VOID SaveUnderCursor(INT32 cx, INT32 cy) {
    for (INT32 r = 0; r < 16; r++)
        for (INT32 c = 0; c < 16; c++) {
            INT32 px = cx + c, py = cy + r;
            if (px >= 0 && py >= 0 && px < (INT32)gScreenWidth && py < (INT32)gScreenHeight)
                gCursorSave[r][c] = gBackBuffer[py * gPixelsPerScanLine + px];
            else
                gCursorSave[r][c] = 0;
        }
    gCursorSaveX = cx;
    gCursorSaveY = cy;
}

STATIC VOID RestoreUnderCursor(VOID) {
    if (gCursorSaveX < 0) return;
    for (INT32 r = 0; r < 16; r++)
        for (INT32 c = 0; c < 16; c++) {
            INT32 px = gCursorSaveX + c, py = gCursorSaveY + r;
            if (px >= 0 && py >= 0 && px < (INT32)gScreenWidth && py < (INT32)gScreenHeight)
                gBackBuffer[py * gPixelsPerScanLine + px] = gCursorSave[r][c];
        }
    MarkDirtyRect(gCursorSaveX, gCursorSaveY, gCursorSaveX + 16, gCursorSaveY + 16);
    gCursorSaveX = -1;
    gCursorSaveY = -1;
}

typedef enum { FILEDLG_NONE, FILEDLG_OPEN, FILEDLG_SAVE } FILEDLG_MODE;
STATIC FILEDLG_MODE gFileDlgMode = FILEDLG_NONE;
STATIC INT32 gFileDlgSelected = -1;
STATIC CHAR16 gFileDlgName[64] = L"";
STATIC UINTN gFileDlgNameLen = 0;
STATIC INT32 gFileDlgPurpose = 0;

#define FD_P_EDITOR_LOAD    1
#define FD_P_EDITOR_SAVE    2
#define FD_P_NOTES_LOAD     3
#define FD_P_NOTES_SAVE     4
#define FD_P_PAINT_LOAD     5
#define FD_P_PAINT_SAVE     6
#define FD_P_IMAGE_LOAD     7
#define FD_P_HEX_LOAD       8
#define FD_P_TEXTTERM_LOAD  9
#define FD_P_IDE_LOAD      10
#define FD_P_IDE_SAVE      11
#define FD_P_SELECT_ONLY   12
#define FD_P_OFFICE_DOC_SAVE    13
#define FD_P_OFFICE_DOC_LOAD    14
#define FD_P_OFFICE_SHEET_SAVE  15
#define FD_P_OFFICE_SHEET_LOAD  16
#define FD_P_OFFICE_SLIDE_SAVE  17
#define FD_P_OFFICE_SLIDE_LOAD  18
#define FD_P_B64_SAVE           19
#define FD_P_B64_LOAD           20

STATIC UINTN gSettingClock24h = 1;
STATIC UINTN gSettingMouseSpeed = 1;
STATIC UINTN gSettingAnimations = 1;
STATIC UINTN gSettingSection = 0;

#define G2048_N 4
#define G2048_CELL 84
STATIC UINT32 g2048Board[G2048_N][G2048_N];
STATIC UINT32 g2048Score = 0, g2048Best = 0;
STATIC BOOLEAN g2048GameOver = FALSE, g2048Won = FALSE;
STATIC UINT32 g2048Rng = 0x12345678;

#define PONG_W 480
#define PONG_H 320
#define PONG_PW 8
#define PONG_PH 60
STATIC INT32 gPongPlayerY = 130, gPongAiY = 130;
STATIC INT32 gPongBallX = 240, gPongBallY = 160;
STATIC INT32 gPongBallVX = 5, gPongBallVY = 4;
STATIC UINT32 gPongScoreP = 0, gPongScoreA = 0;
STATIC BOOLEAN gPongGameOver = FALSE;

#define HEX_BYTES_PER_ROW 16
#define HEX_MAX_DUMP 8192
STATIC UINT8 gHexData[HEX_MAX_DUMP];
STATIC UINTN gHexLen = 0, gHexScroll = 0;
STATIC CHAR16 gHexFileName[64] = L"(no file)";

#define MS_W 12
#define MS_H 12
#define MS_MINES 20
#define MS_CELL 26
STATIC UINT8 gMSBoard[MS_H][MS_W];
STATIC UINT8 gMSState[MS_H][MS_W];
STATIC BOOLEAN gMSGameOver = FALSE, gMSWon = FALSE, gMSFirstClick = TRUE;
STATIC UINT32 gMSRng = 0xC0FFEE;

STATIC UINT32 gCP_R=128, gCP_G=128, gCP_B=128;
STATIC INT32  gCP_ActiveSlider = -1;
STATIC CHAR16 gCP_HexBuf[16] = L"#808080";

#define B64_BUF 4096
STATIC CHAR16 gB64Input[B64_BUF] = L"";
STATIC UINTN  gB64InputLen = 0;
STATIC CHAR16 gB64Output[B64_BUF * 2] = L"";
STATIC UINTN  gB64OutputLen = 0;

/* Hash tool state */
#define HASH_INPUT_MAX 1024
STATIC CHAR16 gHashInput[HASH_INPUT_MAX] = L"";
STATIC UINTN  gHashInputLen = 0;
STATIC CHAR16 gHashMd5Out[33]  = L"";
STATIC CHAR16 gHashSha1Out[41] = L"";

STATIC UINTN   gPwLen = 16;
STATIC BOOLEAN gPwUseUpper = TRUE, gPwUseLower = TRUE, gPwUseDigits = TRUE, gPwUseSymbols = FALSE;
STATIC CHAR16  gPwOutput[64] = L"";
STATIC UINT32  gPwRng = 0xBADC0DE;

STATIC UINTN   gPianoOctave = 4;
STATIC UINT32  gPianoToneFreq = 0;
STATIC UINTN   gPianoToneFrames = 0;
STATIC INT32   gPianoLastKey = -1;

typedef struct { UINT8 Rank; UINT8 Suit; BOOLEAN FaceUp; BOOLEAN Exists; } SOL_CARD;
STATIC SOL_CARD gSolStock[24]; STATIC INT32 gSolStockCount = 0;
STATIC SOL_CARD gSolWaste[24]; STATIC INT32 gSolWasteCount = 0;
STATIC SOL_CARD gSolFound[4][13]; STATIC INT32 gSolFoundCount[4] = {0,0,0,0};
STATIC SOL_CARD gSolTab[7][20]; STATIC INT32 gSolTabCount[7] = {0,0,0,0,0,0,0};
STATIC INT32    gSolSelPile = -1;
STATIC INT32    gSolSelIdx = -1;
STATIC UINT32   gSolRng = 0xDEADBEEF;
STATIC BOOLEAN  gSolWon = FALSE;

VOID ShowStartupScreen(VOID);
VOID ShowShutdownSequence(BOOLEAN IsReboot);
VOID TerminalPrint(IN CONST CHAR16 *Text);
VOID ExecuteCommandString(IN CONST CHAR16 *Cmd, IN BOOLEAN IsShell);
EFI_STATUS SaveFileToFS(IN CHAR16 *FileName, IN VOID *Buffer, IN UINTN BufferSize);
EFI_STATUS LoadFileFromFS(IN CHAR16 *FileName, IN VOID *Buffer, IN OUT UINTN *BufferSize);
VOID BringToFront(UEFI_WINDOW *Win);
VOID FileDialogOpen(FILEDLG_MODE mode, INT32 purpose);
VOID FileDialogRender(VOID);
BOOLEAN FileDialogHandleInput(BOOLEAN Click, BOOLEAN LastClick);

STATIC UEFI_WINDOW **gStoreTargets = NULL;
STATIC CONST CHAR16 **gStoreNames = NULL;
STATIC BOOLEAN *gStoreInstalled = NULL;
STATIC UINTN gStoreAppCount = 0;
STATIC UINTN gStoreAppCapacity = 0;

STATIC BOOLEAN StoreEnsureCapacity(UINTN Needed) {
    if (Needed <= gStoreAppCapacity) return TRUE;
    UINTN NewCap = gStoreAppCapacity ? gStoreAppCapacity * 2 : 16;
    while (NewCap < Needed) NewCap *= 2;
    UEFI_WINDOW **newTargets = NULL;
    CONST CHAR16 **newNames = NULL;
    BOOLEAN *newInstalled = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, NewCap * sizeof(UEFI_WINDOW *), (VOID **)&newTargets))) return FALSE;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, NewCap * sizeof(CONST CHAR16 *), (VOID **)&newNames))) { gBS->FreePool(newTargets); return FALSE; }
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, NewCap * sizeof(BOOLEAN), (VOID **)&newInstalled))) { gBS->FreePool(newTargets); gBS->FreePool(newNames); return FALSE; }
    for (UINTN i = 0; i < gStoreAppCount; i++) {
        newTargets[i] = gStoreTargets[i];
        newNames[i] = gStoreNames[i];
        newInstalled[i] = gStoreInstalled[i];
    }
    if (gStoreTargets) gBS->FreePool(gStoreTargets);
    if (gStoreNames) gBS->FreePool(gStoreNames);
    if (gStoreInstalled) gBS->FreePool(gStoreInstalled);
    gStoreTargets = newTargets;
    gStoreNames = newNames;
    gStoreInstalled = newInstalled;
    gStoreAppCapacity = NewCap;
    return TRUE;
}

STATIC VOID StoreAdd(UEFI_WINDOW *Win, CONST CHAR16 *Name) {
    if (!Win || !Name) return;
    if (!StoreEnsureCapacity(gStoreAppCount + 1)) return;
    gStoreTargets[gStoreAppCount] = Win;
    gStoreNames[gStoreAppCount] = Name;
    gStoreInstalled[gStoreAppCount] = FALSE;
    gStoreAppCount++;
}

VOID RenderEditor(UEFI_WINDOW *Win); BOOLEAN InputEditor(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderPaint(UEFI_WINDOW *Win); BOOLEAN InputPaint(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderFileManager(UEFI_WINDOW *Win); BOOLEAN InputFileManager(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderFindReplace(UEFI_WINDOW *Win); BOOLEAN InputFindReplace(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderAnimEdit(UEFI_WINDOW *Win); BOOLEAN InputAnimEdit(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderAnimView(UEFI_WINDOW *Win); BOOLEAN InputAnimView(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderCalc(UEFI_WINDOW *Win); BOOLEAN InputCalc(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSysInfo(UEFI_WINDOW *Win); BOOLEAN InputSysInfo(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSnake(UEFI_WINDOW *Win); BOOLEAN InputSnake(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderTextTerm(UEFI_WINDOW *Win); BOOLEAN InputTextTerm(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderDiskUtil(UEFI_WINDOW *Win); BOOLEAN InputDiskUtil(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderRoboChat(UEFI_WINDOW *Win); BOOLEAN InputRoboChat(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderTaskManager(UEFI_WINDOW *Win); BOOLEAN InputTaskManager(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID PollNewAppsKeyboard(VOID);
VOID RenderNewWallpaper(VOID);
VOID RenderNotes(UEFI_WINDOW *Win); BOOLEAN InputNotes(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderTexLagApp(UEFI_WINDOW *Win);
BOOLEAN InputTexLagApp(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderMiniIDE(UEFI_WINDOW *Win); BOOLEAN InputMiniIDE(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderHelp(UEFI_WINDOW *Win); BOOLEAN InputHelp(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderTetris(UEFI_WINDOW *Win); BOOLEAN InputTetris(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderCommonApps(UEFI_WINDOW *Win); BOOLEAN InputCommonApps(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSandbox(UEFI_WINDOW *Win);
BOOLEAN InputSandbox(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
);
STATIC VOID UpdateSandbox(VOID);

#define IMAGE_VIEW_W 250
#define IMAGE_VIEW_H 180
STATIC UINT32 gImageViewerCanvas[PAINT_W * PAINT_H];
STATIC CHAR16 gImageViewerFileName[64] = L"Image.pnt";
STATIC BOOLEAN gImageViewerLoaded = FALSE;
STATIC INT32 gImageViewerZoom = 1;

#define UARCH_MAX_FILES 8
#define UARCH_NAME_LEN  64
#define UARCH_MAGIC     0x55415243u
typedef struct { UINT32 Magic; UINT32 FileCount; } UARCH_HEADER;
typedef struct { CHAR16 FileName[UARCH_NAME_LEN]; UINT32 FileSize; } UARCH_ENTRY;
STATIC CHAR16 gArchiveName[64] = L"Bundle.uarch";
STATIC BOOLEAN gArchiveStatusGood = FALSE;

#define CLIPBOARD_HISTORY 8
#define CLIPBOARD_TEXT_MAX 1024
STATIC CHAR16 gClipboardHistory[CLIPBOARD_HISTORY][CLIPBOARD_TEXT_MAX];
STATIC UINTN gClipboardCount = 0;
STATIC INT32 gClipboardSelected = -1;

typedef enum { UNIT_BYTES, UNIT_TEMPERATURE } UNIT_MODE;
STATIC UNIT_MODE gUnitMode = UNIT_BYTES;
STATIC CHAR16 gUnitInput[32] = L"1";
STATIC UINTN gUnitInputLen = 1;

STATIC CHAR16 gSearchQuery[64] = L"";
STATIC UINTN gSearchQueryLen = 0;
STATIC CHAR16 gSearchResults[MAX_FILES][64];
STATIC UINTN gSearchResultCount = 0;
STATIC BOOLEAN gSearchDone = FALSE;

STATIC BOOLEAN gMusicPlaying = FALSE;

STATIC CHAR16 gEasterSequence[] = L"uefitext1337";
STATIC UINTN gEasterSequencePos = 0;
STATIC BOOLEAN gEasterEggUnlocked = FALSE;

VOID ResetTetris(VOID);
VOID UpdateTetris(VOID);
VOID RenderImageViewer(UEFI_WINDOW *Win); BOOLEAN InputImageViewer(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderArchive(UEFI_WINDOW *Win); BOOLEAN InputArchive(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderClipboard(UEFI_WINDOW *Win); BOOLEAN InputClipboard(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderUnitConverter(UEFI_WINDOW *Win); BOOLEAN InputUnitConverter(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSearch(UEFI_WINDOW *Win); BOOLEAN InputSearch(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderMusic(UEFI_WINDOW *Win); BOOLEAN InputMusic(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderEasterEgg(UEFI_WINDOW *Win); BOOLEAN InputEasterEgg(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSettings(UEFI_WINDOW *Win); BOOLEAN InputSettings(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID Render2048(UEFI_WINDOW *Win); BOOLEAN Input2048(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderPong(UEFI_WINDOW *Win); BOOLEAN InputPong(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderHexViewer(UEFI_WINDOW *Win); BOOLEAN InputHexViewer(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderMinesweeper(UEFI_WINDOW *Win); BOOLEAN InputMinesweeper(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderColorPicker(UEFI_WINDOW *Win); BOOLEAN InputColorPicker(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID UpdatePong(VOID);
VOID RenderBase64(UEFI_WINDOW *Win); BOOLEAN InputBase64(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderHash(UEFI_WINDOW *Win); BOOLEAN InputHash(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderPasswordGen(UEFI_WINDOW *Win); BOOLEAN InputPasswordGen(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderPiano(UEFI_WINDOW *Win); BOOLEAN InputPiano(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderSolitaire(UEFI_WINDOW *Win); BOOLEAN InputSolitaire(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID PianoStartNote(UINT32 Freq); VOID PianoStopNote(VOID);
VOID UpdatePianoTick(VOID);
VOID ScarletFireStart(VOID);
VOID ScarletFireStop(VOID);
VOID UpdateScarletFire(VOID);
VOID SolNewGame(VOID);
VOID RenderStore(UEFI_WINDOW *Win); BOOLEAN InputStore(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderBrowser(UEFI_WINDOW *Win); BOOLEAN InputBrowser(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);
VOID RenderOffice(UEFI_WINDOW *Win); BOOLEAN InputOffice(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last);

STATIC UEFI_WINDOW gEditorWin   = {40,40,580,420,40,40,580,420,FALSE,FALSE,FALSE,FALSE,0,0,L"Text Editor",RenderEditor,InputEditor};
STATIC UEFI_WINDOW gPaintWin    = {180,50,480,420,180,50,480,420,FALSE,FALSE,FALSE,FALSE,0,0,L"Paint",RenderPaint,InputPaint};
STATIC UEFI_WINDOW gFileWin     = {260,100,380,300,260,100,380,300,FALSE,FALSE,FALSE,FALSE,0,0,L"File Manager",RenderFileManager,InputFileManager};
STATIC UEFI_WINDOW gFindWin     = {200,150,320,140,200,150,320,140,FALSE,FALSE,FALSE,FALSE,0,0,L"Find & Replace",RenderFindReplace,InputFindReplace};
STATIC UEFI_WINDOW gAnimEditWin = {100,60,400,360,100,60,400,360,FALSE,FALSE,FALSE,FALSE,0,0,L"Animation Editor",RenderAnimEdit,InputAnimEdit};
STATIC UEFI_WINDOW gAnimViewWin = {520,80,280,260,520,80,280,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Animation Viewer",RenderAnimView,InputAnimView};
STATIC UEFI_WINDOW gCalcWin     = {300,180,220,260,300,180,220,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Calculator",RenderCalc,InputCalc};
STATIC UEFI_WINDOW gSysInfoWin  = {150,100,360,220,150,100,360,220,FALSE,FALSE,FALSE,FALSE,0,0,L"System Info",RenderSysInfo,InputSysInfo};
STATIC UEFI_WINDOW gSnakeWin    = {180,120,280,280,180,120,280,280,FALSE,FALSE,FALSE,FALSE,0,0,L"Snake Game",RenderSnake,InputSnake};
STATIC UEFI_WINDOW gTextTermWin = {80,80,480,300,80,80,480,300,FALSE,FALSE,FALSE,FALSE,0,0,L"Terminal",RenderTextTerm,InputTextTerm};
STATIC UEFI_WINDOW gDiskUtilWin = {240,110,420,260,240,110,420,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Disk Utility",RenderDiskUtil,InputDiskUtil};
STATIC UEFI_WINDOW gRoboChatWin = {230,120,440,330,230,120,440,330,FALSE,FALSE,FALSE,FALSE,0,0,L"Robot Chat",RenderRoboChat,InputRoboChat};
STATIC UEFI_WINDOW gTaskMgrWin = {170,60,520,410,170,60,520,410,FALSE,FALSE,FALSE,FALSE,0,0,L"Task Manager",RenderTaskManager,InputTaskManager};
STATIC UEFI_WINDOW gNotesWin = {120,70,520,380,120,70,520,380,FALSE,FALSE,FALSE,FALSE,0,0,L"Notes",RenderNotes,InputNotes};
STATIC UEFI_WINDOW gSandboxWin = {
    180, 70, 620, 470,
    180, 70, 620, 470,
    FALSE, FALSE, FALSE, FALSE, 0, 0,
    L"Sandbox", RenderSandbox, InputSandbox
};

STATIC UEFI_WINDOW gTexLagAppWin = {
    190,90,620,440,190,90,620,440,
    FALSE,FALSE,FALSE,FALSE,0,0,
    L"TexLag App",RenderTexLagApp,InputTexLagApp
};
STATIC UEFI_WINDOW gMiniIDEWin = {70,45,720,500,70,45,720,500,FALSE,FALSE,FALSE,FALSE,0,0,L"Mini IDE",RenderMiniIDE,InputMiniIDE};
STATIC UEFI_WINDOW gHelpWin = {150,45,720,620,150,45,720,620,FALSE,FALSE,FALSE,FALSE,0,0,L"UEFIText Help",RenderHelp,InputHelp};
STATIC UEFI_WINDOW gTetrisWin = {170,60,390,520,170,60,390,520,FALSE,FALSE,FALSE,FALSE,0,0,L"Tetris",RenderTetris,InputTetris};
STATIC UEFI_WINDOW gCommonAppsWin = {210,120,500,320,210,120,500,320,FALSE,FALSE,FALSE,FALSE,0,0,L"Common Apps",RenderCommonApps,InputCommonApps};
STATIC UEFI_WINDOW gImageViewerWin = {160,80,420,320,160,80,420,320,FALSE,FALSE,FALSE,FALSE,0,0,L"Image Viewer",RenderImageViewer,InputImageViewer};
STATIC UEFI_WINDOW gArchiveWin = {180,100,460,300,180,100,460,300,FALSE,FALSE,FALSE,FALSE,0,0,L"File Archive",RenderArchive,InputArchive};
STATIC UEFI_WINDOW gClipboardWin = {190,90,500,330,190,90,500,330,FALSE,FALSE,FALSE,FALSE,0,0,L"Clipboard",RenderClipboard,InputClipboard};
STATIC UEFI_WINDOW gUnitConverterWin = {220,120,400,280,220,120,400,280,FALSE,FALSE,FALSE,FALSE,0,0,L"Unit Converter",RenderUnitConverter,InputUnitConverter};
STATIC UEFI_WINDOW gSearchWin = {150,90,560,360,150,90,560,360,FALSE,FALSE,FALSE,FALSE,0,0,L"Search",RenderSearch,InputSearch};
STATIC UEFI_WINDOW gMusicWin = {220,130,400,250,220,130,400,250,FALSE,FALSE,FALSE,FALSE,0,0,L"PC PIT Music",RenderMusic,InputMusic};
STATIC UEFI_WINDOW gEasterEggWin = {300,180,400,250,300,180,400,250,FALSE,FALSE,FALSE,FALSE,0,0,L"what in the world did u came from? o_o",RenderEasterEgg,InputEasterEgg};
STATIC UEFI_WINDOW gSettingsWin = {180,70,620,470,180,70,620,470,FALSE,FALSE,FALSE,FALSE,0,0,L"Settings",RenderSettings,InputSettings};
STATIC UEFI_WINDOW g2048Win = {300,80,420,500,300,80,420,500,FALSE,FALSE,FALSE,FALSE,0,0,L"2048",Render2048,Input2048};
STATIC UEFI_WINDOW gPongWin = {260,140,520,420,260,140,520,420,FALSE,FALSE,FALSE,FALSE,0,0,L"Pong",RenderPong,InputPong};
STATIC UEFI_WINDOW gHexWin = {120,70,720,500,120,70,720,500,FALSE,FALSE,FALSE,FALSE,0,0,L"Hex Viewer",RenderHexViewer,InputHexViewer};
STATIC UEFI_WINDOW gMSWin = {260,90,420,460,260,90,420,460,FALSE,FALSE,FALSE,FALSE,0,0,L"Minesweeper",RenderMinesweeper,InputMinesweeper};
STATIC UEFI_WINDOW gCPWin = {320,130,400,340,320,130,400,340,FALSE,FALSE,FALSE,FALSE,0,0,L"Color Picker",RenderColorPicker,InputColorPicker};
STATIC UEFI_WINDOW gB64Win = {260,120,500,400,260,120,500,400,FALSE,FALSE,FALSE,FALSE,0,0,L"Base64 Tool",RenderBase64,InputBase64};
STATIC UEFI_WINDOW gHashWin = {220,140,620,320,220,140,620,320,FALSE,FALSE,FALSE,FALSE,0,0,L"Hash Tool",RenderHash,InputHash};
STATIC UEFI_WINDOW gPwWin  = {280,140,440,320,280,140,440,320,FALSE,FALSE,FALSE,FALSE,0,0,L"Password Generator",RenderPasswordGen,InputPasswordGen};
STATIC UEFI_WINDOW gPianoWin = {180,180,660,260,180,180,660,260,FALSE,FALSE,FALSE,FALSE,0,0,L"Piano",RenderPiano,InputPiano};
STATIC UEFI_WINDOW gSolWin = {120,60,790,560,120,60,790,560,FALSE,FALSE,FALSE,FALSE,0,0,L"Solitaire",RenderSolitaire,InputSolitaire};
STATIC UEFI_WINDOW gStoreWin = {200,90,600,460,200,90,600,460,FALSE,FALSE,FALSE,FALSE,0,0,L"Store",RenderStore,InputStore};
STATIC UEFI_WINDOW gBrowserWin = {140,80,700,500,140,80,700,500,FALSE,FALSE,FALSE,FALSE,0,0,L"Browser",RenderBrowser,InputBrowser};
STATIC UEFI_WINDOW gOfficeWin = {100,60,780,540,100,60,780,540,FALSE,FALSE,FALSE,FALSE,0,0,L"Office",RenderOffice,InputOffice};

typedef struct {
    UEFI_WINDOW  *Win;
    CONST CHAR16 *BarName;
    BOOLEAN       InBar;
    BOOLEAN       InMenu;
} APP_REG;

STATIC APP_REG gAppRegistry[] = {
    { &gEditorWin,      L"Editor",    TRUE,  TRUE  },
    { &gPaintWin,       L"Paint",     TRUE,  TRUE  },
    { &gCalcWin,        L"Calc",      TRUE,  TRUE  },
    { &gStoreWin,       L"Store",     TRUE,  TRUE  },
    { &gBrowserWin,     L"Browser",   TRUE,  TRUE  },
    { &gSettingsWin,    L"Settings",  TRUE,  TRUE  },
    { &gFileWin,        L"Files",     TRUE,  TRUE  },
    { &gFindWin,        NULL,         FALSE, FALSE },
    { &gAnimEditWin,    L"AnimE",     TRUE,  FALSE },
    { &gAnimViewWin,    L"AnimV",     TRUE,  FALSE },
    { &gSysInfoWin,     L"SysInfo",   TRUE,  FALSE },
    { &gSnakeWin,       L"Snake",     TRUE,  FALSE },
    { &gTextTermWin,    L"Terminal",  TRUE,  TRUE  },
    { &gDiskUtilWin,    L"Disk",      TRUE,  FALSE },
    { &gRoboChatWin,    L"Robo",      TRUE,  FALSE },
    { &gTaskMgrWin,     L"TaskMgr",   TRUE,  FALSE },
    { &gNotesWin,       L"Notes",     TRUE,  FALSE },
    { &gSandboxWin, L"Sandbox",       TRUE, TRUE   },
    { &gHelpWin,        L"Help",      TRUE,  TRUE  },
    { &gTetrisWin,      L"Tetris",    TRUE,  FALSE },
    { &gCommonAppsWin,  L"Common",    TRUE,  FALSE },
    { &gImageViewerWin, L"ImgView",   TRUE,  FALSE },
    { &gArchiveWin,     L"Archive",   TRUE,  FALSE },
    { &gClipboardWin,   L"Clipboard", TRUE,  FALSE },
    { &gUnitConverterWin, L"Convert", TRUE,  FALSE },
    { &gSearchWin,      L"Search",    TRUE,  FALSE },
    { &gMusicWin,       L"Music",     TRUE,  FALSE },
    { &gEasterEggWin,   NULL,         FALSE, FALSE },
    { &g2048Win,        L"2048",      TRUE,  FALSE },
    { &gPongWin,        L"Pong",      TRUE,  FALSE },
    { &gHexWin,         L"HexView",   TRUE,  FALSE },
    { &gMSWin,          L"Mines",     TRUE,  FALSE },
    { &gCPWin,          L"ColorPick", TRUE,  FALSE },
    { &gB64Win,         L"Base64",    TRUE,  FALSE },
    { &gHashWin,        L"Hash",      TRUE,  FALSE },
    { &gPwWin,          L"PwGen",     TRUE,  FALSE },
    { &gPianoWin,       L"Piano",     TRUE,  FALSE },
    { &gSolWin,         L"Solitaire", TRUE,  FALSE },
    { &gOfficeWin,      L"Office",    TRUE,  FALSE },
};
#define APP_COUNT (sizeof(gAppRegistry) / sizeof(gAppRegistry[0]))

STATIC UEFI_WINDOW *gWinOrder[MAX_WINDOWS];

STATIC VOID InitWindowRegistry(VOID) {
    gWindowCount = 0;

    for (UINTN i = 0; i < APP_COUNT &&
         gWindowCount < MAX_WINDOWS; i++) {
        if (gAppRegistry[i].Win) {
            gWinOrder[gWindowCount++] = gAppRegistry[i].Win;
        }
    }

    /* Sandbox is the permanent bottom window. */
    for (UINTN i = 0; i < gWindowCount; i++) {
        if (gWinOrder[i] == &gSandboxWin) {
            UEFI_WINDOW *Sandbox = gWinOrder[i];

            for (UINTN j = i; j > 0; j--) {
                gWinOrder[j] = gWinOrder[j - 1];
            }

            gWinOrder[0] = Sandbox;
            break;
        }
    }
}

STATIC BOOLEAN IsTopWindow(UEFI_WINDOW *Win) {
    return Win && gWindowCount && gWinOrder[gWindowCount - 1] == Win;
}

STATIC UINT32 JumpscareRandom(VOID) {
    gJumpscareRandom = gJumpscareRandom * 1664525u + 1013904223u;
    return gJumpscareRandom;
}

STATIC UINTN GetTaskbarVisibleCount(VOID) {
    INT32 AvailableWidth = (INT32)gScreenWidth - TASKBAR_START_W - TASKBAR_SCROLL_W * 2
        - TASKBAR_CLOCK_W - TASKBAR_POWER_W - 20;
    if (AvailableWidth <= 0) return 1;
    UINTN Count = (UINTN)(AvailableWidth / TASKBAR_BUTTON_W);
    if (Count == 0) Count = 1;
    return Count;
}

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

STATIC CONST UINT8 FONT_MONO[128][8] = {
    [' ']={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!']={0x10,0x10,0x10,0x10,0x10,0x00,0x10,0x00},
    ['"']={0x28,0x28,0x28,0x00,0x00,0x00,0x00,0x00},
    ['#']={0x28,0x28,0x7C,0x28,0x7C,0x28,0x28,0x00},
    ['$']={0x10,0x3C,0x50,0x38,0x14,0x78,0x10,0x00},
    ['%']={0x44,0xA8,0x50,0x20,0x14,0x2A,0x44,0x00},
    ['&']={0x30,0x48,0x50,0x20,0x54,0x48,0x34,0x00},
    ['\'']={0x10,0x10,0x10,0x00,0x00,0x00,0x00,0x00},
    ['(']={0x08,0x10,0x20,0x20,0x20,0x10,0x08,0x00},
    [')']={0x20,0x10,0x08,0x08,0x08,0x10,0x20,0x00},
    ['*']={0x00,0x28,0x10,0x7C,0x10,0x28,0x00,0x00},
    ['+']={0x00,0x10,0x10,0x7C,0x10,0x10,0x00,0x00},
    [',']={0x00,0x00,0x00,0x00,0x00,0x10,0x10,0x20},
    ['-']={0x00,0x00,0x00,0x7C,0x00,0x00,0x00,0x00},
    ['.']={0x00,0x00,0x00,0x00,0x00,0x10,0x10,0x00},
    ['/']={0x04,0x08,0x10,0x20,0x40,0x80,0x00,0x00},
    ['0']={0x38,0x44,0x4C,0x54,0x64,0x44,0x38,0x00},
    ['1']={0x10,0x30,0x10,0x10,0x10,0x10,0x38,0x00},
    ['2']={0x38,0x44,0x04,0x18,0x20,0x40,0x7C,0x00},
    ['3']={0x38,0x44,0x04,0x18,0x04,0x44,0x38,0x00},
    ['4']={0x08,0x18,0x28,0x48,0x7C,0x08,0x08,0x00},
    ['5']={0x7C,0x40,0x78,0x04,0x04,0x44,0x38,0x00},
    ['6']={0x38,0x40,0x78,0x44,0x44,0x44,0x38,0x00},
    ['7']={0x7C,0x04,0x08,0x10,0x20,0x20,0x20,0x00},
    ['8']={0x38,0x44,0x44,0x38,0x44,0x44,0x38,0x00},
    ['9']={0x38,0x44,0x44,0x3C,0x04,0x04,0x38,0x00},
    [':']={0x00,0x10,0x10,0x00,0x10,0x10,0x00,0x00},
    [';']={0x00,0x10,0x10,0x00,0x10,0x10,0x20,0x00},
    ['<']={0x08,0x10,0x20,0x40,0x20,0x10,0x08,0x00},
    ['=']={0x00,0x00,0x7C,0x00,0x7C,0x00,0x00,0x00},
    ['>']={0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x00},
    ['?']={0x38,0x44,0x04,0x18,0x10,0x00,0x10,0x00},
    ['@']={0x38,0x44,0x5C,0x54,0x5C,0x40,0x38,0x00},
    ['A']={0x10,0x28,0x44,0x44,0x7C,0x44,0x44,0x00},
    ['B']={0x78,0x44,0x44,0x78,0x44,0x44,0x78,0x00},
    ['C']={0x38,0x44,0x40,0x40,0x40,0x44,0x38,0x00},
    ['D']={0x78,0x44,0x44,0x44,0x44,0x44,0x78,0x00},
    ['E']={0x7C,0x40,0x40,0x78,0x40,0x40,0x7C,0x00},
    ['F']={0x7C,0x40,0x40,0x78,0x40,0x40,0x40,0x00},
    ['G']={0x38,0x44,0x40,0x5C,0x44,0x44,0x38,0x00},
    ['H']={0x44,0x44,0x44,0x7C,0x44,0x44,0x44,0x00},
    ['I']={0x7C,0x10,0x10,0x10,0x10,0x10,0x7C,0x00},
    ['J']={0x1C,0x08,0x08,0x08,0x08,0x48,0x30,0x00},
    ['K']={0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00},
    ['L']={0x40,0x40,0x40,0x40,0x40,0x40,0x7C,0x00},
    ['M']={0x44,0x6C,0x54,0x54,0x44,0x44,0x44,0x00},
    ['N']={0x44,0x44,0x64,0x54,0x4C,0x44,0x44,0x00},
    ['O']={0x38,0x44,0x44,0x44,0x44,0x44,0x38,0x00},
    ['P']={0x78,0x44,0x44,0x78,0x40,0x40,0x40,0x00},
    ['Q']={0x38,0x44,0x44,0x44,0x54,0x48,0x34,0x00},
    ['R']={0x78,0x44,0x44,0x78,0x50,0x48,0x44,0x00},
    ['S']={0x38,0x44,0x40,0x38,0x04,0x44,0x38,0x00},
    ['T']={0x7C,0x10,0x10,0x10,0x10,0x10,0x10,0x00},
    ['U']={0x44,0x44,0x44,0x44,0x44,0x44,0x38,0x00},
    ['V']={0x44,0x44,0x44,0x44,0x44,0x28,0x10,0x00},
    ['W']={0x44,0x44,0x44,0x54,0x54,0x6C,0x44,0x00},
    ['X']={0x44,0x44,0x28,0x10,0x28,0x44,0x44,0x00},
    ['Y']={0x44,0x44,0x28,0x10,0x10,0x10,0x10,0x00},
    ['Z']={0x7C,0x04,0x08,0x10,0x20,0x40,0x7C,0x00},
    ['[']={0x38,0x20,0x20,0x20,0x20,0x20,0x38,0x00},
    ['\\']={0x80,0x40,0x20,0x10,0x08,0x04,0x00,0x00},
    [']']={0x38,0x08,0x08,0x08,0x08,0x08,0x38,0x00},
    ['^']={0x10,0x28,0x44,0x00,0x00,0x00,0x00,0x00},
    ['_']={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE},
    ['`']={0x20,0x10,0x00,0x00,0x00,0x00,0x00,0x00},
    ['a']={0x00,0x00,0x38,0x04,0x3C,0x44,0x3C,0x00},
    ['b']={0x40,0x40,0x78,0x44,0x44,0x44,0x78,0x00},
    ['c']={0x00,0x00,0x38,0x40,0x40,0x40,0x38,0x00},
    ['d']={0x04,0x04,0x3C,0x44,0x44,0x44,0x3C,0x00},
    ['e']={0x00,0x00,0x38,0x44,0x7C,0x40,0x38,0x00},
    ['f']={0x18,0x20,0x78,0x20,0x20,0x20,0x20,0x00},
    ['g']={0x00,0x00,0x3C,0x44,0x44,0x3C,0x04,0x38},
    ['h']={0x40,0x40,0x78,0x44,0x44,0x44,0x44,0x00},
    ['i']={0x10,0x00,0x30,0x10,0x10,0x10,0x38,0x00},
    ['j']={0x08,0x00,0x18,0x08,0x08,0x08,0x48,0x30},
    ['k']={0x40,0x40,0x48,0x50,0x60,0x50,0x48,0x00},
    ['l']={0x30,0x10,0x10,0x10,0x10,0x10,0x38,0x00},
    ['m']={0x00,0x00,0x68,0x54,0x54,0x54,0x54,0x00},
    ['n']={0x00,0x00,0x78,0x44,0x44,0x44,0x44,0x00},
    ['o']={0x00,0x00,0x38,0x44,0x44,0x44,0x38,0x00},
    ['p']={0x00,0x00,0x78,0x44,0x44,0x78,0x40,0x40},
    ['q']={0x00,0x00,0x3C,0x44,0x44,0x3C,0x04,0x04},
    ['r']={0x00,0x00,0x58,0x64,0x40,0x40,0x40,0x00},
    ['s']={0x00,0x00,0x3C,0x40,0x38,0x04,0x78,0x00},
    ['t']={0x20,0x20,0x78,0x20,0x20,0x24,0x18,0x00},
    ['u']={0x00,0x00,0x44,0x44,0x44,0x44,0x3C,0x00},
    ['v']={0x00,0x00,0x44,0x44,0x44,0x28,0x10,0x00},
    ['w']={0x00,0x00,0x44,0x44,0x54,0x54,0x28,0x00},
    ['x']={0x00,0x00,0x44,0x28,0x10,0x28,0x44,0x00},
    ['y']={0x00,0x00,0x44,0x44,0x44,0x3C,0x04,0x38},
    ['z']={0x00,0x00,0x7C,0x08,0x10,0x20,0x7C,0x00},
    ['{']={0x0C,0x10,0x10,0x60,0x10,0x10,0x0C,0x00},
    ['|']={0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00},
    ['}']={0x60,0x10,0x10,0x0C,0x10,0x10,0x60,0x00},
    ['~']={0x00,0x00,0x48,0x54,0x24,0x00,0x00,0x00}
};


typedef enum {
    FONT_STYLE_SANS = 0,
    FONT_STYLE_BOLD = 1,
    FONT_STYLE_MONO = 2,
    FONT_STYLE_COMIC = 3,
    FONT_STYLE_8x8  = FONT_STYLE_SANS,
    FONT_STYLE_8x16 = FONT_STYLE_BOLD,
    FONT_STYLE_5x7  = FONT_STYLE_MONO
} FONT_STYLE;
STATIC FONT_STYLE gFontStyle = FONT_STYLE_8x8;
STATIC UINTN      gFontScale = 1;

STATIC INT32 CharW(VOID) {
    INT32 w = 8;
    if (gFontStyle == FONT_STYLE_BOLD) w = 9;
    return w * (INT32)gFontScale;
}
STATIC INT32 CharH(VOID) {
    return 8 * (INT32)gFontScale;
}
STATIC INT32 TextW(CONST CHAR16 *s) {
    if (!s) return 0;
    return (INT32)StrLen(s) * CharW();
}

STATIC VOID StoreInit(VOID) {
    gStoreAppCount = 0;
    StoreAdd(&gNotesWin,      L"Notes");
    StoreAdd(&gOfficeWin,     L"UEFIText Office");
    StoreAdd(&gTetrisWin,     L"Tetris");
    StoreAdd(&gSnakeWin,      L"Snake");
    StoreAdd(&g2048Win,       L"2048");
    StoreAdd(&gPongWin,       L"Pong");
    StoreAdd(&gMSWin,         L"Minesweeper");
    StoreAdd(&gSolWin,        L"Solitaire");
    StoreAdd(&gMusicWin,      L"PC Speaker Music");
    StoreAdd(&gPianoWin,      L"Piano");
	StoreAdd(&gHashWin,       L"Hash Tool");
}


STATIC BOOLEAN IsInRect(INT32 X, INT32 Y, INT32 RX, INT32 RY, INT32 RW, INT32 RH) {
    return (X >= RX && X < RX + RW && Y >= RY && Y < RY + RH);
}

STATIC VOID FastDrawPixel(INT32 x, INT32 y, UINT32 color) {
    if (x >= gClipX0 && y >= gClipY0 && x < gClipX1 && y < gClipY1) {
        MarkDirtyTile(x >> TILE_SHIFT, y >> TILE_SHIFT);
        gBackBuffer[y * gPixelsPerScanLine + x] = color;
    }
}

VOID DrawRect(INT32 x, INT32 y, INT32 w, INT32 h, UINT32 color) {
    if (w <= 0 || h <= 0) return;
    INT32 x0 = x < gClipX0 ? gClipX0 : x;
    INT32 y0 = y < gClipY0 ? gClipY0 : y;
    INT32 x1 = x + w; if (x1 > gClipX1) x1 = gClipX1;
    INT32 y1 = y + h; if (y1 > gClipY1) y1 = gClipY1;
    INT32 rw = x1 - x0;
    if (rw <= 0) return;
    MarkDirtyRect(x0, y0, x1, y1);
    for (INT32 r = y0; r < y1; r++) {
        UINT32 *row = &gBackBuffer[r * gPixelsPerScanLine + x0];
        for (INT32 c = 0; c < rw; c++) row[c] = color;
    }
}

STATIC VOID SetClip(INT32 x, INT32 y, INT32 w, INT32 h) {
    gClipX0 = x < 0 ? 0 : x;
    gClipY0 = y < 0 ? 0 : y;
    gClipX1 = x + w; if (gClipX1 > (INT32)gScreenWidth)  gClipX1 = (INT32)gScreenWidth;
    gClipY1 = y + h; if (gClipY1 > (INT32)gScreenHeight) gClipY1 = (INT32)gScreenHeight;
    if (gClipX0 > gClipX1) gClipX0 = gClipX1;
    if (gClipY0 > gClipY1) gClipY0 = gClipY1;
}

STATIC VOID ResetClip(VOID) {
    gClipX0 = 0; gClipY0 = 0;
    gClipX1 = (INT32)gScreenWidth;
    gClipY1 = (INT32)gScreenHeight;
}

VOID DrawChar(CHAR16 ch, INT32 x, INT32 y, UINT32 color) {
    if (ch > 127) ch = '?';
    INT32 sc = (INT32)gFontScale;
    CONST UINT8 *glyph = (gFontStyle == FONT_STYLE_MONO) ? FONT_MONO[ch] : FONT_8x8[ch];

    if (gFontStyle != FONT_STYLE_BOLD && sc == 1
        && x >= gClipX0 && y >= gClipY0
        && x + 8 <= gClipX1 && y + 8 <= gClipY1) {
        MarkDirtyRect(x, y, x + 8, y + 8);
        for (INT32 r = 0; r < 8; r++) {
            UINT8 row = glyph[r];
            UINT32 *dst = &gBackBuffer[(y + r) * gPixelsPerScanLine + x];
            for (INT32 c = 0; c < 8; c++)
                if (row & (1 << (7 - c))) dst[c] = color;
        }
        return;
    }

    UINT8 buf[8][9];
    INT32 cols = 8;
    for (INT32 r = 0; r < 8; r++)
        for (INT32 c = 0; c < 9; c++)
            buf[r][c] = 0;

    if (gFontStyle == FONT_STYLE_BOLD) {
        cols = 9;
        for (INT32 r = 0; r < 8; r++) {
            UINT8 row = glyph[r];
            for (INT32 c = 0; c < 8; c++) {
                if (row & (1 << (7 - c))) {
                    buf[r][c] = 1;
                    buf[r][c + 1] = 1;
                }
            }
        }
    } else if (gFontStyle == FONT_STYLE_COMIC) {
        for (INT32 r = 0; r < 8; r++) {
            UINT8 row = glyph[r];
            UINT8 wide = (UINT8)(row | (row << 1) | (row >> 1));
            for (INT32 c = 0; c < 8; c++)
                if (wide & (1 << (7 - c))) buf[r][c] = 1;
        }
    } else {
        for (INT32 r = 0; r < 8; r++) {
            UINT8 row = glyph[r];
            for (INT32 c = 0; c < 8; c++)
                if (row & (1 << (7 - c))) buf[r][c] = 1;
        }
    }

    for (INT32 r = 0; r < 8; r++) {
        for (INT32 c = 0; c < cols; c++) {
            if (!buf[r][c]) continue;
            for (INT32 sy = 0; sy < sc; sy++)
                for (INT32 sx = 0; sx < sc; sx++)
                    FastDrawPixel(x + c*sc + sx, y + r*sc + sy, color);
        }
    }
}

VOID DrawString(IN CONST CHAR16 *str, INT32 x, INT32 y, UINT32 color) {
    INT32 cw = CharW();
    while (*str) { DrawChar(*str, x, y, color); x += cw; str++; }
}

VOID DrawButton(INT32 x, INT32 y, INT32 w, INT32 h, CONST CHAR16 *label, UINT32 bg, UINT32 fg) {
    DrawRect(x, y, w, h, bg);
    DrawString(label, x + (w - TextW(label)) / 2, y + (h - CharH()) / 2, fg);
}

STATIC VOID CalibrateTsc(VOID) {
    UINT64 c0 = read_tsc();
    gBS->Stall(5000000);
    UINT64 c1 = read_tsc();
    gTscPerUs = (c1 - c0) / 5000000;
    if (gTscPerUs == 0) gTscPerUs = 1000;
    gFpsWindowStart = read_tsc();
}

STATIC VOID FrameDelayTsc(UINT32 Us) {
    if (Us == 0) return;
    if (gTscPerUs == 0) { gBS->Stall(Us); return; }
    UINT64 end = read_tsc() + (UINT64)Us * gTscPerUs;
    while (read_tsc() < end) { }
}

STATIC VOID CopyTileRow(UINT32 *dst, CONST UINT32 *src, INT32 tw) {
    INT32 i = 0;
    while (i < tw && (((UINTN)(dst + i)) & 0xF) != 0) { dst[i] = src[i]; i++; }
    INT32 quads = (tw - i) >> 2;
    for (INT32 q = 0; q < quads; q++)
        _mm_stream_si128((__m128i *)(dst + i + q*4),
                         _mm_loadu_si128((CONST __m128i *)(src + i + q*4)));
    for (INT32 j = i + quads * 4; j < tw; j++) dst[j] = src[j];
}

VOID SwapBuffers(VOID) {
    if (gBackBuffer == gFrameBuffer) return;

    INT32 W = (INT32)gScreenWidth;
    INT32 H = (INT32)gScreenHeight;

    for (INT32 ty = 0; ty < MAX_TILES_Y; ty++) {
        INT32 tileY = ty << TILE_SHIFT;
        if (tileY >= H) break;
        INT32 th = TILE_SIZE;
        if (tileY + th > H) th = H - tileY;
        INT32 rowBits = ty * MAX_TILES_X;
        for (INT32 tx = 0; tx < MAX_TILES_X; tx++) {
            INT32 tileX = tx << TILE_SHIFT;
            if (tileX >= W) break;
            INT32 bit = rowBits + tx;
            if (!(gDirtyBits[bit >> 3] & (UINT8)(1u << (bit & 7)))) continue;
            INT32 tw = TILE_SIZE;
            if (tileX + tw > W) tw = W - tileX;
            for (INT32 r = 0; r < th; r++) {
                UINT32 *src = &gBackBuffer[(tileY + r) * gPixelsPerScanLine + tileX];
                UINT32 *dst = &gFrameBuffer[(tileY + r) * gPixelsPerScanLine + tileX];
                CopyTileRow(dst, src, tw);
            }
        }
    }

    _mm_sfence();
    gBS->SetMem(gDirtyBits, sizeof(gDirtyBits), 0);
}

VOID PlayTone(UINT32 FreqHz, UINT32 DurationMs) {
    if (!gSoundEnabled) { gBS->Stall(DurationMs * 1000); return; }
    if (FreqHz == 0)    { gBS->Stall(DurationMs * 1000); return; }
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
    PlayTone(523,100); PlayTone(659,100); PlayTone(784,100); PlayTone(1047,300);
}
VOID PlayShutdownJingle(VOID) {
    PlayTone(784,120); PlayTone(659,120); PlayTone(523,120); PlayTone(392,300);
}

VOID ShowStartupScreen(VOID) {
    INT32 W = (INT32)gScreenWidth;
    INT32 H = (INT32)gScreenHeight;
    INT32 CX = W / 2;
    INT32 CY = H / 2;

    FONT_STYLE saveStyle = gFontStyle;
    UINTN saveScale = gFontScale;
    gFontStyle = FONT_STYLE_BOLD;
    gFontScale = 3;

    CONST UINT32 BG = RGB(255,255,255);
    CONST UINT32 FG = RGB(20,20,20);
    CONST UINT32 SK = RGB(90,90,90);

    CONST UINT32 FRAME_US = 1000000 / 35;
    CONST INT32  TOTAL_FRAMES = 140;
    CONST INT32  PHASE_LEN = 35;

    INT32 DotX[4] = { W/6, W*5/6, W/4, W*7/8 };
    INT32 DotY[4] = { H/6, H/4,   H*3/4, H*5/6 };

    for (INT32 f = 0; f < TOTAL_FRAMES; f++) {
        DrawRect(0, 0, W, H, BG);

        INT32 phase = f / PHASE_LEN;
        INT32 sub   = f % PHASE_LEN;

        for (INT32 i = 0; i < 4; i++) {
            INT32 appearAt = i * 6;
            INT32 sz = 4;
            if (phase == 0) {
                if (sub < appearAt) continue;
                INT32 age = sub - appearAt;
                sz = (age < 6) ? (2 + age) : 4;
            }
            DrawRect(DotX[i] - sz/2, DotY[i] - sz/2, sz, sz, FG);
        }

        INT32 len1 = 0, len2 = 0, len3 = 0;
        if (phase == 1) {
            INT32 p = sub * 100 / (PHASE_LEN - 1);
            len1 = 60 * p / 100;
            len2 = 40 * p / 100;
            len3 = 40 * p / 100;
        } else if (phase >= 2) {
            len1 = 60; len2 = 40; len3 = 40;
        }
        if (len1 > 0) DrawRect(W/5, H/4, 3, len1, SK);
        if (len2 > 0) DrawRect(W/5, H/4, len2, 3, SK);
        if (len3 > 0) DrawRect(W/4, H*3/4, 3, len3, SK);

        INT32 welcomeY = CY - 40;
        if (phase == 2) {
            welcomeY = CY + 60 - (100 * sub / (PHASE_LEN - 1));
        } else if (phase == 3) {
            welcomeY = CY - 40 - (40 * sub / (PHASE_LEN - 1));
        }
        if (phase >= 2) {
            INT32 tw = TextW(L"Welcome to");
            DrawString(L"Welcome to", CX - tw/2, welcomeY, FG);
        }

        if (phase == 3) {
            INT32 tw2 = TextW(L"UefiText! ._.");
            INT32 tw3 = TextW(L"'it said'");
            UINT32 c2 = FG;
            if (sub < 17) {
                UINT32 bright = 220 - (sub * 200 / 17);
                c2 = RGB(bright, bright, bright);
            }
            DrawString(L"UefiText! ._.", CX - tw2/2, CY, c2);
            if (sub >= 17) {
                DrawString(L"'it said'", CX - tw3/2, CY + 90, SK);
            }
        }

        SwapBuffers();
        gBS->Stall(FRAME_US);
    }

    gFontStyle = saveStyle;
    gFontScale = saveScale;

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
    PlayShutdownJingle();
    UINT32 AmberColor = RGB(245,158,11);
    for (INT32 seconds = 10; seconds >= 0; seconds--) {
        DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));
        if (IsReboot) {
            DrawString(L"Rebooting...",CenterX-56,CenterY-30,AmberColor);
            DrawString(L"(If slow after reboot, power-cycle)",CenterX-160,CenterY-10,AmberColor);
        } else {
            DrawString(L"It is now safe to turn off",CenterX-104,CenterY-30,AmberColor);
            DrawString(L"Wait 10s before powering back on",CenterX-144,CenterY-10,AmberColor);
        }
        CHAR16 TimerMsg[64];
        if (IsReboot) UnicodeSPrint(TimerMsg,sizeof(TimerMsg),L"Rebooting in %d seconds...",seconds);
        else          UnicodeSPrint(TimerMsg,sizeof(TimerMsg),L"Powering off in %d seconds...",seconds);
        DrawString(TimerMsg,CenterX-((INT32)StrLen(TimerMsg)*CharW()/2),CenterY+40,RGB(100,116,139));
        SwapBuffers();
        if (seconds > 0) { PlayTone(880,50); gBS->Stall(1000000); }
        else             { gBS->Stall(1000000); }
    }
    
    gRT->ResetSystem(IsReboot ? EfiResetCold : EfiResetShutdown,EFI_SUCCESS,0,NULL);
}

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
    while (Text[i] != L'\0' && i < TERM_COLS-1) { gTermBuffer[gTermLineCount][i] = Text[i]; i++; }
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
        Root->Close(Root); return;
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
            gTextTermBuffer[0]=L'\0'; gTextTermLen=0;
            StrCpyS(gTextTermStatus,64,L"New file created.");
        } else {
            gTextTermLen=ReadSize/sizeof(CHAR16);
            if (gTextTermLen >= MAX_TEXT) gTextTermLen=MAX_TEXT-1;
            gTextTermBuffer[gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"File loaded successfully.");
        }
    } else {
        StrCpyS(gTextTermFileName,64,L"untitled");
        gTextTermBuffer[0]=L'\0'; gTextTermLen=0;
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

STATIC VOID ShellAmberPoweroff(BOOLEAN IsReboot) {
    gShellCmdBuf[0] = L'\0'; gShellCmdLen = 0;
    for (INT32 sec = 10; sec >= 0; sec--) {
        DrawRect(0, (INT32)gScreenHeight - 60, (INT32)gScreenWidth, 60, RGB(0,0,0));
        CHAR16 Msg[64];
        if (IsReboot) UnicodeSPrint(Msg, sizeof(Msg), L"Rebooting in %d seconds...", sec);
        else          UnicodeSPrint(Msg, sizeof(Msg), L"Powering off in %d seconds...", sec);
        DrawString(Msg, 10, (INT32)gScreenHeight - 46, RGB(245,158,11));
        if (IsReboot)
            DrawString(L"(If slow after reboot, power-cycle)", 10, (INT32)gScreenHeight - 26, RGB(245,158,11));
        else
            DrawString(L"It is now safe to turn off", 10, (INT32)gScreenHeight - 26, RGB(245,158,11));
        SwapBuffers();
        gBS->Stall(1000000);
    }
    gRT->ResetSystem(IsReboot ? EfiResetCold : EfiResetShutdown, EFI_SUCCESS, 0, NULL);
}

STATIC VOID RunMatrixRain(VOID) {
    STATIC CONST UINT32 MatrixColors[32] = {
        RGB(128,0,0),     RGB(170,0,0),     RGB(128,64,0),    RGB(170,85,0),
        RGB(128,128,0),   RGB(170,170,0),   RGB(0,128,0),     RGB(0,170,0),
        RGB(0,128,128),   RGB(0,170,170),   RGB(0,0,128),     RGB(0,0,170),
        RGB(128,0,128),   RGB(170,0,170),   RGB(128,128,128), RGB(192,192,192),
        RGB(255,0,0),     RGB(255,85,0),    RGB(255,170,0),   RGB(255,255,0),
        RGB(170,255,0),   RGB(85,255,0),    RGB(0,255,0),     RGB(0,255,85),
        RGB(0,255,170),   RGB(0,255,255),   RGB(0,170,255),   RGB(0,85,255),
        RGB(0,0,255),     RGB(85,0,255),    RGB(170,0,255),   RGB(255,0,255)
    };
    INT32 Drops[256];
    UINTN Columns = (UINTN)gScreenWidth / (UINTN)CharW();
    if (Columns > 256) Columns = 256;
    if (Columns == 0) return;

    UINT32 Seed = (UINT32)(gScreenWidth ^ (gScreenHeight << 1) ^ 0xA5C31E27U);
    for (UINTN i = 0; i < Columns; i++)
        Drops[i] = -(INT32)((i * 17U) % (gScreenHeight + 1U));

    while (TRUE) {
        DrawRect(0, 0, (INT32)gScreenWidth, (INT32)gScreenHeight, RGB(0,0,0));
        for (UINTN i = 0; i < Columns; i++) {
            INT32 X = (INT32)i * CharW();
            for (INT32 Tail = 0; Tail < 7; Tail++) {
                INT32 Y = Drops[i] - Tail * (CharH() + 1);
                if (Y < 0 || Y >= (INT32)gScreenHeight) continue;
                Seed = Seed * 1664525U + 1013904223U;
                CHAR16 Glyph = L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"[Seed % 62U];
                UINT32 Ink = MatrixColors[Seed % 32U];
                DrawChar(Glyph, X, Y, Ink);
            }
            Seed = Seed * 1664525U + 1013904223U;
            Drops[i] += CharH() + 1 + (INT32)(Seed % 3U);
            if (Drops[i] > (INT32)gScreenHeight + 7 * (CharH() + 1))
                Drops[i] = -(INT32)(Seed % (gScreenHeight + 1U));
        }
        DrawString(L"32-COLOR MATRIX RAIN - Press ESC to return", 10, 10, MatrixColors[15]);
        SwapBuffers();

        EFI_INPUT_KEY Key;
        if (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_SUCCESS &&
            (Key.ScanCode == SCAN_ESC || Key.UnicodeChar == 0x1B))
            break;
        gBS->Stall(50000);
    }
    DrawRect(0, 0, (INT32)gScreenWidth, (INT32)gScreenHeight, RGB(0,0,0));
    SwapBuffers();
}

STATIC VOID StartGuiMatrixRain(VOID) {
    INT32 CanvasH = gTextTermWin.H - 42;
    INT32 CanvasW = gTextTermWin.W - 16;
    INT32 StepY = CharH() + 1;

    if (CanvasH < 1) CanvasH = 1;
    if (CanvasW < 1) CanvasW = 1;
    if (StepY < 1) StepY = 1;

    UINTN Rows = (UINTN)(CanvasH / StepY) + 1;
    if (Rows == 0) Rows = 1;

    UINTN Columns = (UINTN)(CanvasW / CharW());
    if (Columns > GUI_MATRIX_MAX_COLUMNS)
        Columns = GUI_MATRIX_MAX_COLUMNS;

    gGuiMatrixSeed =
        (UINT32)(gScreenWidth ^ (gScreenHeight << 1) ^ 0xA5C31E27U);

    for (UINTN I = 0; I < GUI_MATRIX_MAX_COLUMNS; I++) {
        gGuiMatrixDrops[I] =
            -(INT32)(((I * 17U) % Rows) * (UINTN)StepY);
    }

    gGuiMatrixActive = TRUE;
    gTextTermWin.IsVisible = TRUE;
    gTextTermWin.IsMinimized = FALSE;
    BringToFront(&gTextTermWin);
    gRedrawWindows = TRUE;
}

STATIC VOID RenderGuiMatrix(
    INT32 X, INT32 Y, INT32 W, INT32 H
) {
    STATIC CONST UINT32 MatrixColors[32] = {
        RGB(128,0,0), RGB(170,0,0), RGB(128,64,0), RGB(170,85,0),
        RGB(128,128,0), RGB(170,170,0), RGB(0,128,0), RGB(0,170,0),
        RGB(0,128,128), RGB(0,170,170), RGB(0,0,128), RGB(0,0,170),
        RGB(128,0,128), RGB(170,0,170), RGB(128,128,128), RGB(192,192,192),
        RGB(255,0,0), RGB(255,85,0), RGB(255,170,0), RGB(255,255,0),
        RGB(170,255,0), RGB(85,255,0), RGB(0,255,0), RGB(0,255,85),
        RGB(0,255,170), RGB(0,255,255), RGB(0,170,255), RGB(0,85,255),
        RGB(0,0,255), RGB(85,0,255), RGB(170,0,255), RGB(255,0,255)
    };

    CONST CHAR16 *Glyphs =
        L"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

    DrawRect(X, Y, W, H, RGB(0,0,0));

    INT32 StepX = CharW();
    INT32 StepY = CharH() + 1;

    if (StepX < 1 || StepY < 1 || W <= 0 || H <= 0)
        return;

    UINTN Columns = (UINTN)(W / StepX);
    if (Columns > GUI_MATRIX_MAX_COLUMNS)
        Columns = GUI_MATRIX_MAX_COLUMNS;

    for (UINTN I = 0; I < Columns; I++) {
        INT32 DrawX = X + (INT32)I * StepX;

        for (INT32 Tail = 0; Tail < 7; Tail++) {
            INT32 DrawY = Y + gGuiMatrixDrops[I] - Tail * StepY;

            if (DrawY < Y || DrawY + CharH() > Y + H)
                continue;

            gGuiMatrixSeed =
                gGuiMatrixSeed * 1664525U + 1013904223U;

            CHAR16 Glyph = Glyphs[gGuiMatrixSeed % 62U];
            UINT32 Ink = MatrixColors[gGuiMatrixSeed % 32U];

            DrawChar(Glyph, DrawX, DrawY, Ink);
        }

        gGuiMatrixSeed =
            gGuiMatrixSeed * 1664525U + 1013904223U;

        gGuiMatrixDrops[I] += StepY + (INT32)(gGuiMatrixSeed % 3U);

        if (gGuiMatrixDrops[I] > H + 7 * StepY) {
            gGuiMatrixDrops[I] =
                -(INT32)(gGuiMatrixSeed % (UINT32)(H + 1));
        }
    }

    DrawString(
        L"MATRIX RAIN - ESC TO STOP",
        X + 6, Y + 5, MatrixColors[15]
    );
}

VOID ExecuteCommandString(IN CONST CHAR16 *Cmd, IN BOOLEAN IsShell) {
    VOID (*PrintFunc)(IN CONST CHAR16*) = IsShell ? ShellPrint : TerminalPrint;
    PrintFunc(Cmd);
    if (StrnCmp(Cmd,L"textterm",8)==0) {
        CONST CHAR16 *Arg=Cmd+8;
        while (*Arg==L' ') Arg++;
        OpenTextTermEditor(Arg);
        if (!IsShell) {
            gTextTermWin.IsVisible=TRUE; gTextTermWin.IsMinimized=FALSE;
            BringToFront(&gTextTermWin);
            gInShellMode = FALSE;
        }
    } else if (StrCmp(Cmd,L"gui")==0) {
        if (IsShell) { gInShellMode=FALSE; gDesktopDirty=TRUE; }
        else PrintFunc(L"GUI already running.");
    } else if (StrCmp(Cmd,L"poweroff")==0) {
        if (IsShell) { PrintFunc(L"Powering off..."); ShellAmberPoweroff(FALSE); }
        else ShowShutdownSequence(FALSE);
    } else if (StrCmp(Cmd,L"matrix")==0) {
        if (IsShell) {
            PrintFunc(L"Matrix rain started. Press ESC to return.");
            RunMatrixRain();
        } else {
            StartGuiMatrixRain();
            PrintFunc(L"GUI Matrix active. Focus Terminal and press ESC to stop.");
    }
    } else if (StrCmp(Cmd,L"reboot")==0) {
        if (IsShell) { PrintFunc(L"Rebooting..."); ShellAmberPoweroff(TRUE); }
        else ShowShutdownSequence(TRUE);
    } else if (StrCmp(Cmd,L"version")==0)  { PrintFunc(L"My UefiText version is v1.0-x64)"); }
    else if (StrCmp(Cmd,L"clear")==0) {
        if (IsShell) gShellLogCount=0; else gTermLineCount=0;
    } else if (StrCmp(Cmd,L"ls")==0) {
        RefreshFileList();
        if (gFileCount==0) PrintFunc(L"No files found.");
        else for (UINTN i=0;i<gFileCount;i++) PrintFunc(gFileList[i]);
    } else if (StrnCmp(Cmd,L"filemake ",9)==0) {
        CONST CHAR16 *FileName=Cmd+9;
        if (StrLen(FileName)>0) {
            CHAR16 DummyData=L'\0';
            EFI_STATUS Status=SaveFileToFS((CHAR16*)FileName,&DummyData,0);
            if (!EFI_ERROR(Status)) { PrintFunc(L"File created successfully."); RefreshFileList(); }
            else PrintFunc(L"Error: Failed to create file.");
        } else PrintFunc(L"Usage: filemake <filename>");
    } else if (StrCmp(Cmd,L"help")==0) {
        gHelpScroll = 0;

        if (gInShellMode) {
            gShellHelpActive = TRUE;
        } else {
            gHelpWin.IsVisible = TRUE;
            gHelpWin.IsMinimized = FALSE;
            BringToFront(&gHelpWin);
            gRedrawWindows = TRUE;
        }
    } else {
        PrintFunc(L"Unknown command. Type 'help'");
    }
}

VOID ExecuteShellCommand(VOID) {
    if (gShellCmdLen==0) return;
    ExecuteCommandString(gShellCmdBuf,TRUE);
    gShellCmdBuf[0]=L'\0'; gShellCmdLen=0;
}

VOID RenderShellMode(VOID) {
    if (gInTextTermEditor) {
        DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));
        CHAR16 Header[128];
        UnicodeSPrint(Header,sizeof(Header),L"TextTerm Editor - [%s]",gTextTermFileName);
        DrawString(Header,10,10,RGB(34,197,94));
        DrawString(L"ESC = save & exit   |   Enter = newline   |   Backspace = delete",10,24,RGB(148,163,184));
        INT32 CurX=10, CurY=44;
        for (UINTN i=0;i<gTextTermLen;i++) {
            if (gTextTermBuffer[i]==L'\n') { CurX=10; CurY+=CharH()+4; continue; }
            DrawChar(gTextTermBuffer[i],CurX,CurY,COLOR_TEXT);
            CurX+=CharW();
            if (CurX+CharW()>=gScreenWidth-10) { CurX=10; CurY+=CharH()+4; }
        }
        DrawRect(CurX,CurY,CharW(),CharH()+2,RGB(34,197,94));
        DrawString(gTextTermStatus,10,gScreenHeight-20,RGB(234,179,8));
        return;
    }

    if (gShellHelpActive) {
        DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));

        DrawString(L"UEFITEXT SHELL HELP",
                   10,10,RGB(34,197,94));
        DrawString(L"SPACE: next page | BACKSPACE: previous | ESC: return",
                   10,28,RGB(148,163,184));

        UINTN PageCount =
            (HELP_LINE_COUNT + SHELL_HELP_LINES_PER_PAGE - 1) /
            SHELL_HELP_LINES_PER_PAGE;
        UINTN Page = gHelpScroll / SHELL_HELP_LINES_PER_PAGE;
        UINTN Start = Page * SHELL_HELP_LINES_PER_PAGE;

        INT32 LineGap = CharH() + 4;
        INT32 Y = 62;

        for (UINTN i = 0;
             i < SHELL_HELP_LINES_PER_PAGE &&
             Start + i < HELP_LINE_COUNT;
             i++) {
            CONST CHAR16 *Line = gHelpLines[Start + i];

            if (Line[0] == L'\0') {
                Y += LineGap;
                continue;
            }

            UINT32 Ink =
                (Start + i == 0 || Line[0] == L'[')
                ? RGB(34,197,94)
                : RGB(241,245,249);

            DrawString(Line,10,Y,Ink);
            Y += LineGap;
        }

        CHAR16 PageText[48];
        UnicodeSPrint(PageText,sizeof(PageText),
                      L"Page %u / %u",
                      (UINT32)(Page + 1),
                      (UINT32)PageCount);

        DrawString(PageText,10,
                   (INT32)gScreenHeight - 22,
                   RGB(34,197,94));
        return;
    }

    DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));
    DrawString(L"Welcome to the Shell! :>",10,10,RGB(34,197,94));
    DrawString(L"Type 'textterm <file>' for text editor or 'gui' to switch desktop.",10,24,RGB(148,163,184));
    INT32 Y=50;
    for (UINTN i=0;i<gShellLogCount;i++) { DrawString(gShellLog[i],10,Y,RGB(241,245,249)); Y+=14; }
    DrawString(L"> ",10,Y,RGB(34,197,94));
    DrawString(gShellCmdBuf,26,Y,RGB(255,255,255));
    DrawRect(26+(INT32)gShellCmdLen*CharW(),Y,CharW(),CharH()+2,RGB(34,197,94));
}

STATIC VOID KeepWindowOnScreen(UEFI_WINDOW *Win) {
    if (!Win || Win->IsMaximized) return;
    INT32 MaxBottom = (INT32)gScreenHeight - TASKBAR_H - 4;
    INT32 MaxRight = (INT32)gScreenWidth - 4;
    if (Win->X < 4) Win->X = 4;
    if (Win->Y < 30) Win->Y = 30;
    if (Win->X + Win->W > MaxRight) Win->X = MaxRight - Win->W;
    if (Win->Y + Win->H > MaxBottom) Win->Y = MaxBottom - Win->H;
    if (Win->X < 4) Win->X = 4;
    if (Win->Y < 30) Win->Y = 30;
}

STATIC VOID ResizeTaskManagerToFit(UEFI_WINDOW *Win) {
    if (!Win || Win->IsMaximized) return;
    INT32 NeededHeight = 26 + 36 + 38 + ((INT32)gWindowCount * 15) + 50;
    INT32 MaxHeight = (INT32)gScreenHeight - TASKBAR_H - Win->Y - 4;
    if (NeededHeight > MaxHeight) NeededHeight = MaxHeight;
    if (NeededHeight < 260) NeededHeight = 260;
    Win->H = NeededHeight;
    KeepWindowOnScreen(Win);
}

STATIC VOID ResizeFileManagerToFit(UEFI_WINDOW *Win) {
    if (!Win || Win->IsMaximized) return;
    INT32 NeededHeight = 64 + ((INT32)gFileCount * 18) + 20;
    if (NeededHeight < 300) NeededHeight = 300;
    INT32 MaxHeight = (INT32)gScreenHeight - TASKBAR_H - Win->Y - 4;
    if (NeededHeight > MaxHeight) NeededHeight = MaxHeight;
    Win->H = NeededHeight;
    KeepWindowOnScreen(Win);
}

#define STORE_HEADER_H    36
#define STORE_ROW_TOP_PAD 46
#define STORE_ROW_H       34
#define STORE_BUTTON_H    46

STATIC VOID StoreResizeWindow(VOID) {
    INT32 NeededH = STORE_HEADER_H + STORE_ROW_TOP_PAD
                  + ((INT32)gStoreAppCount * STORE_ROW_H)
                  + STORE_BUTTON_H;
    INT32 MaxH = (INT32)gScreenHeight - TASKBAR_H - gStoreWin.Y - 8;
    if (NeededH > MaxH) NeededH = MaxH;
    if (NeededH < 220) NeededH = 220;
    gStoreWin.H = NeededH;
    gStoreWin.OldH = NeededH;
    KeepWindowOnScreen(&gStoreWin);
}

VOID BringToFront(UEFI_WINDOW *Win) {
    if (!Win) return;

    /* First, bring the clicked window to the front normally. */
    INT32 Idx = -1;

    for (UINTN I = 0; I < gWindowCount; I++) {
        if (gWinOrder[I] == Win) {
            Idx = (INT32)I;
            break;
        }
    }

    if (Idx >= 0 && Idx < (INT32)gWindowCount - 1) {
        for (UINTN I = (UINTN)Idx; I < gWindowCount - 1; I++) {
            gWinOrder[I] = gWinOrder[I + 1];
        }

        gWinOrder[gWindowCount - 1] = Win;
    }

    /*
     * If another window was selected, move Sandbox
     * to the bottom. Clicking Sandbox itself leaves
     * it in the foreground.
     */
    if (Win != &gSandboxWin) {
        INT32 SandboxIdx = -1;

        for (UINTN I = 0; I < gWindowCount; I++) {
            if (gWinOrder[I] == &gSandboxWin) {
                SandboxIdx = (INT32)I;
                break;
            }
        }

        if (SandboxIdx > 0) {
            UEFI_WINDOW *Sandbox = gWinOrder[SandboxIdx];

            for (INT32 I = SandboxIdx; I > 0; I--) {
                gWinOrder[I] = gWinOrder[I - 1];
            }

            gWinOrder[0] = Sandbox;
        }
    }
}

VOID ToggleWindowMaximize(UEFI_WINDOW *Win) {
    if (!Win->IsMaximized) {
        Win->OldX=Win->X; Win->OldY=Win->Y; Win->OldW=Win->W; Win->OldH=Win->H;
        Win->X=0; Win->Y=26; Win->W=gScreenWidth; Win->H=gScreenHeight-26;
        Win->IsMaximized=TRUE;
    } else {
        Win->X=Win->OldX; Win->Y=Win->OldY; Win->W=Win->OldW; Win->H=Win->OldH;
        Win->IsMaximized=FALSE;
    }
}

STATIC BOOLEAN HasCustomChrome(UEFI_WINDOW *Win) {
    if (!Win) return FALSE;
    if (Win == &gNotesWin) return TRUE;
    return FALSE;
}

VOID DrawWindowTitleBar(UEFI_WINDOW *Win) {
    if (!Win->IsVisible || Win->IsMinimized) return;
    if (HasCustomChrome(Win)) return;
    INT32 TitleH=26, BtnW=24;
    DrawRect(Win->X,Win->Y+TitleH,Win->W,Win->H-TitleH,COLOR_WIN_BG);
    DrawRect(Win->X,Win->Y,Win->W,TitleH,COLOR_TITLE_ACTIVE);
    DrawString(Win->Title,Win->X+8,Win->Y+7,COLOR_TEXT);
    INT32 CloseX=Win->X+Win->W-BtnW;
    INT32 MaxX=CloseX-BtnW;
    INT32 MinX=MaxX-BtnW;
    DrawButton(MinX,Win->Y,BtnW,TitleH,L"-",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(MaxX, Win->Y, BtnW, TitleH, L"", COLOR_TOOLBAR_BG, COLOR_TEXT);
    {
        INT32 Sx = MaxX + (BtnW - 8) / 2;
        INT32 Sy = Win->Y + (TitleH - 8) / 2;
        DrawRect(Sx,     Sy,     8, 1, COLOR_TEXT);
        DrawRect(Sx,     Sy + 7, 8, 1, COLOR_TEXT);
        DrawRect(Sx,     Sy,     1, 8, COLOR_TEXT);
        DrawRect(Sx + 7, Sy,     1, 8, COLOR_TEXT);
    }
    DrawButton(CloseX,Win->Y,BtnW,TitleH,L"X",COLOR_BTN_CLOSE,COLOR_TEXT);
}

EFI_STATUS SaveFileToFS(IN CHAR16 *FileName, IN VOID *Buffer, IN UINTN BufferSize) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS=NULL;
    EFI_FILE_PROTOCOL *Root=NULL,*File=NULL;
    EFI_STATUS Status=gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid,NULL,(VOID**)&FS);
    if (EFI_ERROR(Status) || EFI_ERROR(FS->OpenVolume(FS,&Root))) return Status;
    Status=Root->Open(Root,&File,FileName,EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE,0);
    if (!EFI_ERROR(Status)) File->Delete(File);
    Status=Root->Open(Root,&File,FileName,
        EFI_FILE_MODE_READ|EFI_FILE_MODE_WRITE|EFI_FILE_MODE_CREATE, EFI_FILE_ARCHIVE);
    if (!EFI_ERROR(Status)) {
        UINTN WriteSize=BufferSize;
        if (WriteSize>0) File->Write(File,&WriteSize,Buffer);
        File->Flush(File); File->Close(File);
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
    if (!EFI_ERROR(Status)) { File->Read(File,BufferSize,Buffer); File->Close(File); }
    Root->Close(Root);
    return Status;
}

VOID FileDialogOpen(FILEDLG_MODE mode, INT32 purpose) {
    RefreshFileList();
    gFileDlgMode = mode;
    gFileDlgSelected = -1;
    gFileDlgName[0] = L'\0';
    gFileDlgNameLen = 0;
    gFileDlgPurpose = purpose;
}

STATIC VOID FileDialogSaveCurrent(VOID) {
    if (gFileDlgNameLen == 0) return;
    switch (gFileDlgPurpose) {
		case FD_P_EDITOR_SAVE:
			if (!EFI_ERROR(SaveFileToFS(
					gFileDlgName,
					gTextBuffer,
					gTextLen * sizeof(CHAR16)))) {
				StrCpyS(gTextFileName, 64, gFileDlgName);
			}
			break;
        case FD_P_NOTES_SAVE:  SaveFileToFS(gFileDlgName, gNotesBuffer, gNotesLen * sizeof(CHAR16)); break;
        case FD_P_PAINT_SAVE:  SaveFileToFS(gFileDlgName, gPaintCanvas, sizeof(gPaintCanvas)); break;
        case FD_P_IDE_SAVE:    SaveFileToFS(gFileDlgName, gIDEBuffer,   gIDELen * sizeof(CHAR16)); break;
        case FD_P_OFFICE_DOC_SAVE:   SaveFileToFS(gFileDlgName, gOfficeDocBuf,   gOfficeDocLen * sizeof(CHAR16)); break;
        case FD_P_OFFICE_SHEET_SAVE: SaveFileToFS(gFileDlgName, gOfficeSheetBuf, gOfficeSheetLen * sizeof(CHAR16)); break;
        case FD_P_OFFICE_SLIDE_SAVE: SaveFileToFS(gFileDlgName, gOfficeSlideBuf, gOfficeSlideLen * sizeof(CHAR16)); break;
        case FD_P_B64_SAVE:          SaveFileToFS(gFileDlgName, gB64Output,      gB64OutputLen * sizeof(CHAR16)); break;
    }
    RefreshFileList();
}

STATIC VOID FileDialogLoadSelected(VOID) {
    if (gFileDlgSelected < 0 || gFileDlgSelected >= (INT32)gFileCount) return;
    CHAR16 *name = gFileList[gFileDlgSelected];
    switch (gFileDlgPurpose) {
        case FD_P_EDITOR_LOAD:
        case FD_P_NOTES_LOAD:
        case FD_P_TEXTTERM_LOAD:
        case FD_P_IDE_LOAD:
        {
            CHAR16 *dst = gTextBuffer;
            UINTN  cap = MAX_TEXT;
            if (gFileDlgPurpose == FD_P_NOTES_LOAD)    { dst = gNotesBuffer; cap = NOTES_MAX_TEXT; }
            else if (gFileDlgPurpose == FD_P_IDE_LOAD) { dst = gIDEBuffer;   cap = IDE_MAX_TEXT; }
            UINTN ReadSize = cap * sizeof(CHAR16);
            if (!EFI_ERROR(LoadFileFromFS(name, dst, &ReadSize))) {
                UINTN n = ReadSize / sizeof(CHAR16);
                if (n >= cap) n = cap - 1;
                dst[n] = L'\0';
                if (gFileDlgPurpose == FD_P_EDITOR_LOAD) {
					gTextLen = n;
					StrCpyS(gTextFileName, 64, name);
					gEditorWin.IsVisible = TRUE;
					gEditorWin.IsMinimized = FALSE;
					BringToFront(&gEditorWin);
				}
                if (gFileDlgPurpose == FD_P_NOTES_LOAD)    { gNotesLen = n; gNotesWin.IsVisible   = TRUE; gNotesWin.IsMinimized   = FALSE; BringToFront(&gNotesWin);   }
                if (gFileDlgPurpose == FD_P_IDE_LOAD)      { gIDELen   = n; gMiniIDEWin.IsVisible = TRUE; gMiniIDEWin.IsMinimized = FALSE; BringToFront(&gMiniIDEWin); }
                if (gFileDlgPurpose == FD_P_TEXTTERM_LOAD) { gTextTermLen = n; StrCpyS(gTextTermFileName, 64, name); gInTextTermEditor = TRUE; gTextTermWin.IsVisible = TRUE; gTextTermWin.IsMinimized = FALSE; gInShellMode = FALSE; BringToFront(&gTextTermWin); }
            }
            break;
        }
        case FD_P_PAINT_LOAD:
        case FD_P_IMAGE_LOAD:
        {
            UINTN ReadSize = sizeof(gPaintCanvas);
            if (!EFI_ERROR(LoadFileFromFS(name, gPaintCanvas, &ReadSize))) {
                gImageViewerLoaded = TRUE;
                StrCpyS(gImageViewerFileName, 64, name);
                if (gFileDlgPurpose == FD_P_PAINT_LOAD) { gPaintWin.IsVisible = TRUE; gPaintWin.IsMinimized = FALSE; BringToFront(&gPaintWin); }
                else                                    { gImageViewerWin.IsVisible = TRUE; gImageViewerWin.IsMinimized = FALSE; BringToFront(&gImageViewerWin); }
            }
            break;
        }
        case FD_P_HEX_LOAD:
        {
            UINTN ReadSize = HEX_MAX_DUMP;
            if (!EFI_ERROR(LoadFileFromFS(name, gHexData, &ReadSize))) {
                gHexLen = ReadSize;
                StrCpyS(gHexFileName, 64, name);
                gHexScroll = 0;
                gHexWin.IsVisible = TRUE; gHexWin.IsMinimized = FALSE;
                BringToFront(&gHexWin);
            }
            break;
        }
        case FD_P_OFFICE_DOC_LOAD:
        case FD_P_OFFICE_SHEET_LOAD:
        case FD_P_OFFICE_SLIDE_LOAD:
        {
            CHAR16 *dst = gOfficeDocBuf;
            UINTN  cap = OFFICE_DOC_MAX;
            if (gFileDlgPurpose == FD_P_OFFICE_SHEET_LOAD) { dst = gOfficeSheetBuf; cap = OFFICE_SHEET_MAX; }
            if (gFileDlgPurpose == FD_P_OFFICE_SLIDE_LOAD) { dst = gOfficeSlideBuf; cap = OFFICE_SLIDE_MAX; }
            UINTN ReadSize = cap * sizeof(CHAR16);
            if (!EFI_ERROR(LoadFileFromFS(name, dst, &ReadSize))) {
                UINTN n = ReadSize / sizeof(CHAR16);
                if (n >= cap) n = cap - 1;
                dst[n] = L'\0';
                if (gFileDlgPurpose == FD_P_OFFICE_DOC_LOAD)   gOfficeDocLen   = n;
                if (gFileDlgPurpose == FD_P_OFFICE_SHEET_LOAD) gOfficeSheetLen = n;
                if (gFileDlgPurpose == FD_P_OFFICE_SLIDE_LOAD) gOfficeSlideLen = n;
            }
            break;
        }
        case FD_P_SELECT_ONLY:
            StrCpyS(gFileDlgName, 64, name);
            break;
    }
}

VOID FileDialogRender(VOID) {
    if (gFileDlgMode == FILEDLG_NONE) return;
    INT32 W = 560, H = 400;
    INT32 X = ((INT32)gScreenWidth - W) / 2;
    INT32 Y = ((INT32)gScreenHeight - H) / 2;

    ResetClip();
    SetClip(X, Y, W, H);
    DrawRect(X, Y, W, H, COLOR_WIN_BG);
    DrawRect(X, Y, W, 26, COLOR_TITLE_ACTIVE);
    DrawString(gFileDlgMode == FILEDLG_OPEN ? L"Open File" : L"Save File",
               X + 8, Y + 8, COLOR_TEXT);

    INT32 LY = Y + 34;
    INT32 LH = H - 34 - 60;
    DrawRect(X + 8, LY, W - 16, LH, COLOR_CANVAS);
    for (UINTN i = 0, ry = LY + 6; i < gFileCount && ry + 16 < (UINTN)(LY + LH); i++, ry += 18) {
        UINT32 bg = (gFileDlgSelected == (INT32)i) ? COLOR_TITLE_ACTIVE : COLOR_TOOLBAR_BG;
        DrawRect(X + 12, ry, W - 24, 17, bg);
        DrawString(gFileList[i], X + 16, ry + 4, COLOR_TEXT);
    }

    if (gFileDlgMode == FILEDLG_SAVE) {
        DrawString(L"Name:", X + 8, Y + H - 54, COLOR_TEXT);
        DrawRect(X + 60, Y + H - 58, W - 200, 22, COLOR_CANVAS);
        DrawString(gFileDlgName, X + 64, Y + H - 54, COLOR_TEXT);
        DrawRect(X + 64 + (INT32)gFileDlgNameLen * CharW(), Y + H - 54, CharW(), CharH() + 2, COLOR_CURSOR);
    }

    DrawButton(X + W - 190, Y + H - 34, 85, 26, L"OK", NEW_APP_COLOR_GOOD, COLOR_TEXT);
    DrawButton(X + W - 100, Y + H - 34, 85, 26, L"Cancel", COLOR_BTN_CLOSE, COLOR_TEXT);
    ResetClip();
}

BOOLEAN FileDialogHandleInput(BOOLEAN Click, BOOLEAN LastClick) {
    if (gFileDlgMode == FILEDLG_NONE) return FALSE;

    INT32 W = 560, H = 400;
    INT32 X = ((INT32)gScreenWidth - W) / 2;
    INT32 Y = ((INT32)gScreenHeight - H) / 2;

    EFI_INPUT_KEY Key;
    if (gST->ConIn->ReadKeyStroke(gST->ConIn, &Key) == EFI_SUCCESS) {
        if (Key.ScanCode == SCAN_ESC) { gFileDlgMode = FILEDLG_NONE; return TRUE; }
        if (gFileDlgMode == FILEDLG_SAVE) {
            if (Key.UnicodeChar == 0x0D || Key.UnicodeChar == 0x0A) {
                FileDialogSaveCurrent();
                gFileDlgMode = FILEDLG_NONE;
                return TRUE;
            } else if (Key.UnicodeChar == 0x08) {
                if (gFileDlgNameLen > 0) { gFileDlgName[--gFileDlgNameLen] = L'\0'; }
            } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E && gFileDlgNameLen < 63) {
                gFileDlgName[gFileDlgNameLen++] = Key.UnicodeChar;
                gFileDlgName[gFileDlgNameLen] = L'\0';
            }
            return TRUE;
        }
    }

    if (!Click || LastClick) return TRUE;

    INT32 LY = Y + 34;
    INT32 LH = H - 34 - 60;
    for (UINTN i = 0, ry = LY + 6; i < gFileCount && ry + 16 < (UINTN)(LY + LH); i++, ry += 18) {
        if (IsInRect(gMouseX, gMouseY, X + 12, ry, W - 24, 17)) {
            gFileDlgSelected = (INT32)i;
            if (gFileDlgMode == FILEDLG_SAVE) {
                StrCpyS(gFileDlgName, 64, gFileList[i]);
                gFileDlgNameLen = StrLen(gFileDlgName);
            }
            return TRUE;
        }
    }

    if (IsInRect(gMouseX, gMouseY, X + W - 190, Y + H - 34, 85, 26)) {
        if (gFileDlgMode == FILEDLG_SAVE) FileDialogSaveCurrent();
        else FileDialogLoadSelected();
        gFileDlgMode = FILEDLG_NONE;
        return TRUE;
    }
    if (IsInRect(gMouseX, gMouseY, X + W - 100, Y + H - 34, 85, 26)) {
        gFileDlgMode = FILEDLG_NONE;
        return TRUE;
    }
    return TRUE;
}


STATIC VOID ResetDefaults(VOID) {
    gThemeDesktop     = RGB(30, 41, 59);
    gThemeTopbarBg    = RGB(15, 23, 42);
    gThemeWinBg       = RGB(51, 65, 85);
    gThemeWinBorder   = RGB(100, 116, 139);
    gThemeCanvas      = RGB(15, 15, 15);
    gThemeTitleActive = RGB(37, 99, 235);
    gThemeBtnClose    = RGB(225, 29, 72);
    gThemeText        = RGB(241, 245, 249);
    gThemeCursor      = RGB(59, 130, 246);
    gThemeToolbarBg   = RGB(30, 58, 138);
    gThemeMenuBg      = RGB(30, 41, 59);
    gThemeMinimapBg   = RGB(20, 25, 35);
    gFrameDelayUs     = 33333;
    gFontStyle        = FONT_STYLE_8x8;
    gFontScale        = 1;
    gSoundEnabled     = FALSE; // true but for later bc my dad argue me
    gWallpaperMode    = 0;
    gWallpaperColor1  = RGB(30,41,59);
    gWallpaperColor2  = RGB(15,23,42);
    gWallpaperCacheDirty = TRUE;
    gDesktopDirty = TRUE;
}

STATIC VOID ApplyThemePreset(UINTN which) {
    switch (which) {
        case 0: /* Slate */
            gThemeDesktop = RGB(30,41,59);  gThemeTopbarBg = RGB(15,23,42);
            gThemeWinBg = RGB(51,65,85);    gThemeWinBorder = RGB(100,116,139);
            gThemeTitleActive = RGB(37,99,235); gThemeBtnClose = RGB(225,29,72);
            gThemeText = RGB(241,245,249);  gThemeCursor = RGB(59,130,246);
            gThemeToolbarBg = RGB(30,58,138); gThemeMenuBg = RGB(30,41,59);
            gThemeMinimapBg = RGB(20,25,35);
            break;
        case 1: /* Nord */
            gThemeDesktop = RGB(46,52,64);  gThemeTopbarBg = RGB(35,39,48);
            gThemeWinBg = RGB(59,66,82);    gThemeWinBorder = RGB(76,86,106);
            gThemeTitleActive = RGB(94,129,172); gThemeBtnClose = RGB(191,97,106);
            gThemeText = RGB(236,239,244);  gThemeCursor = RGB(136,192,208);
            gThemeToolbarBg = RGB(76,86,106); gThemeMenuBg = RGB(59,66,82);
            gThemeMinimapBg = RGB(35,39,48);
            break;
        case 2: /* Dracula */
            gThemeDesktop = RGB(40,42,54);  gThemeTopbarBg = RGB(30,31,41);
            gThemeWinBg = RGB(68,71,90);    gThemeWinBorder = RGB(98,114,164);
            gThemeTitleActive = RGB(189,147,249); gThemeBtnClose = RGB(255,85,85);
            gThemeText = RGB(248,248,242);  gThemeCursor = RGB(139,233,253);
            gThemeToolbarBg = RGB(68,71,90); gThemeMenuBg = RGB(40,42,54);
            gThemeMinimapBg = RGB(30,31,41);
            break;
        case 3: /* Paper */
            gThemeDesktop = RGB(238,232,213); gThemeTopbarBg = RGB(253,246,227);
            gThemeWinBg = RGB(238,232,213);   gThemeWinBorder = RGB(147,161,161);
            gThemeTitleActive = RGB(38,139,210); gThemeBtnClose = RGB(220,50,47);
            gThemeText = RGB(7,54,66);        gThemeCursor = RGB(42,161,152);
            gThemeToolbarBg = RGB(238,232,213); gThemeMenuBg = RGB(253,246,227);
            gThemeMinimapBg = RGB(238,232,213);
            break;
    }
    /* Canvas contrast: light text gets dark canvas, dark text gets light canvas. */
    if (gThemeText > 0x800000) gThemeCanvas = RGB(15,15,15);
    else                       gThemeCanvas = RGB(250,248,240);
    gWallpaperCacheDirty = TRUE;
    gDesktopDirty = TRUE;
}

VOID ExecuteFindReplace(VOID) {
    if (gFindLen==0 || gTextLen==0) return;
    CHAR16 NewText[MAX_TEXT];
    UINTN NewLen=0;
    for (UINTN i=0;i<gTextLen;) {
        BOOLEAN Match=(i+gFindLen<=gTextLen);
        for (UINTN j=0;Match && j<gFindLen;j++) if (gTextBuffer[i+j]!=gFindBuf[j]) Match=FALSE;
        if (Match) {
            for (UINTN j=0;j<gReplaceLen && NewLen<MAX_TEXT-1;j++) NewText[NewLen++]=gReplaceBuf[j];
            i+=gFindLen;
        } else if (NewLen<MAX_TEXT-1) NewText[NewLen++]=gTextBuffer[i++];
        else i++;
    }
    NewText[NewLen]=L'\0';
    gTextLen=NewLen;
    gBS->CopyMem(gTextBuffer,NewText,(gTextLen+1)*sizeof(CHAR16));
}

VOID ResetSnakeGame(VOID) {
    gSnakeLength=3;
    gSnakeBody[0].X=5; gSnakeBody[0].Y=5;
    gSnakeBody[1].X=4; gSnakeBody[1].Y=5;
    gSnakeBody[2].X=3; gSnakeBody[2].Y=5;
    gSnakeDirX=1; gSnakeDirY=0;
    gSnakeFood.X=12; gSnakeFood.Y=8;
    gSnakeGameOver=FALSE; gSnakeScore=0;
}

VOID UpdateSnakeGame(VOID) {
    if (gSnakeGameOver) return;
    POINT NewHead={gSnakeBody[0].X+gSnakeDirX,gSnakeBody[0].Y+gSnakeDirY};
    if (NewHead.X<0 || NewHead.X>=SNAKE_GRID_W || NewHead.Y<0 || NewHead.Y>=SNAKE_GRID_H) { gSnakeGameOver=TRUE; return; }
    for (UINTN i=0;i<gSnakeLength;i++)
        if (gSnakeBody[i].X==NewHead.X && gSnakeBody[i].Y==NewHead.Y) { gSnakeGameOver=TRUE; return; }
    for (UINTN i=gSnakeLength;i>0;i--) gSnakeBody[i]=gSnakeBody[i-1];
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
    DrawRect(BoardX+(gSnakeFood.X*SNAKE_CELL_SZ)+1,BoardY+(gSnakeFood.Y*SNAKE_CELL_SZ)+1,
             SNAKE_CELL_SZ-2,SNAKE_CELL_SZ-2,RGB(225,29,72));
    for (UINTN i=0;i<gSnakeLength;i++) {
        UINT32 SnakeColor=(i==0)?RGB(34,197,94):RGB(134,239,172);
        DrawRect(BoardX+(gSnakeBody[i].X*SNAKE_CELL_SZ)+1,BoardY+(gSnakeBody[i].Y*SNAKE_CELL_SZ)+1,
                 SNAKE_CELL_SZ-2,SNAKE_CELL_SZ-2,SnakeColor);
    }
    if (gSnakeGameOver) DrawString(L"GAME OVER! Press Reset",BoardX+10,BoardY+80,RGB(255,75,70));
    DrawButton(CanvasX,BoardY+(SNAKE_GRID_H*SNAKE_CELL_SZ)+10,80,22,L"Reset",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawString(L"Controls: Arrow Keys",CanvasX+90,BoardY+(SNAKE_GRID_H*SNAKE_CELL_SZ)+16,COLOR_TEXT);
}

BOOLEAN InputSnake(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+12,CanvasY=Win->Y+34;
    INT32 BoardY=CanvasY+16;
    if (Click && !Last && IsInRect(MX,MY,CanvasX,BoardY+(SNAKE_GRID_H*SNAKE_CELL_SZ)+10,80,22))
        ResetSnakeGame();
    return TRUE;
}

VOID RenderTextTerm(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    INT32 CanvasW=Win->W-16,CanvasH=Win->H-42;
    DrawRect(CanvasX,CanvasY,CanvasW,CanvasH,COLOR_CANVAS);

    if (gGuiMatrixActive && !gInTextTermEditor) {
        RenderGuiMatrix(CanvasX,CanvasY,CanvasW,CanvasH);
        return;
    }

    if (gInTextTermEditor) {
        CHAR16 Header[128];
        UnicodeSPrint(Header,sizeof(Header),L"TextTerm Editor - [%s]",gTextTermFileName);
        DrawString(Header,CanvasX+8,CanvasY+6,RGB(34,197,94));
        DrawString(L"Press ESC to exit editor | Ctrl+S saving active",CanvasX+8,CanvasY+18,RGB(148,163,184));
        DrawRect(CanvasX+8,CanvasY+30,CanvasW-16,1,COLOR_WIN_BORDER);
        INT32 CurX=CanvasX+8,CurY=CanvasY+36;
        for (UINTN i=0;i<gTextTermLen;i++) {
            if (gTextTermBuffer[i]==L'\n') { CurX=CanvasX+8; CurY+=CharH()+4; continue; }
            DrawChar(gTextTermBuffer[i],CurX,CurY,COLOR_TEXT);
            CurX+=CharW();
            if (CurX+CharW()>=CanvasX+CanvasW-16) { CurX=CanvasX+8; CurY+=CharH()+4; }
        }
        DrawRect(CurX,CurY,CharW(),CharH()+2,RGB(34,197,94));
        DrawString(gTextTermStatus,CanvasX+8,CanvasY+CanvasH-16,RGB(234,179,8));
    } else {
        DrawString(L"TextTerm Shell (Type 'textterm <file>' to make and EDIT a file.) ()",CanvasX+8,CanvasY+6,RGB(34,197,94));
        INT32 CurY=CanvasY+22;
        for (UINTN i=0;i<gShellLogCount;i++,CurY+=CharH()+4)
            DrawString(gShellLog[i],CanvasX+8,CurY,RGB(241,245,249));
        DrawString(L"# ",CanvasX+8,CurY,RGB(34,197,94));
        DrawString(gShellCmdBuf,CanvasX+24,CurY,RGB(255,255,255));
        DrawRect(CanvasX+24+(INT32)gShellCmdLen*CharW(),CurY,CharW(),CharH()+2,RGB(34,197,94));
    }
}

BOOLEAN InputTextTerm(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) { return TRUE; }

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
                    BlockIo->Media->RemovableMedia?L"Removable":L"Fixed", BlockIo->Media->MediaId);
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
    UnicodeSPrint(Buf,sizeof(Buf),L"Root FS Files: %d present",gFileCount);
    DrawString(Buf,CanvasX,ItemY,RGB(34,197,94));
}

BOOLEAN InputDiskUtil(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) { return TRUE; }

VOID RenderCalc(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    DrawRect(CanvasX,CanvasY,Win->W-16,30,COLOR_CANVAS);
    DrawString(gCalcDisplay,CanvasX+Win->W-24-TextW(gCalcDisplay),CanvasY+10,COLOR_TEXT);
    CONST CHAR16 *Buttons[]={ L"7",L"8",L"9",L"/",L"4",L"5",L"6",L"*",L"1",L"2",L"3",L"-",L"C",L"0",L"=",L"+" };
    INT32 StartY=CanvasY+40;
    for (INT32 r=0;r<4;r++) for (INT32 c=0;c<4;c++)
        DrawButton(CanvasX+(c*50),StartY+(r*42),45,38,Buttons[r*4+c],COLOR_TOOLBAR_BG,COLOR_TEXT);
}

VOID HandleCalcAction(CONST CHAR16 *Label) {
    if (Label[0]>=L'0' && Label[0]<=L'9') {
        if (gCalcNewEntry || StrCmp(gCalcDisplay,L"0")==0) {
            gCalcDisplay[0]=Label[0]; gCalcDisplay[1]=L'\0'; gCalcNewEntry=FALSE;
        } else if (StrLen(gCalcDisplay)<12) {
            UINTN len=StrLen(gCalcDisplay);
            gCalcDisplay[len]=Label[0]; gCalcDisplay[len+1]=L'\0';
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
        gCalcOp=L'\0'; gCalcNewEntry=TRUE;
    } else {
        gCalcVal1=(INT32)StrDecimalToUintn(gCalcDisplay);
        gCalcOp=Label[0]; gCalcNewEntry=TRUE;
    }
}

BOOLEAN InputCalc(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    if (Click && !Last) {
        INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34,StartY=CanvasY+40;
        CONST CHAR16 *Buttons[]={ L"7",L"8",L"9",L"/",L"4",L"5",L"6",L"*",L"1",L"2",L"3",L"-",L"C",L"0",L"=",L"+" };
        for (INT32 r=0;r<4;r++) for (INT32 c=0;c<4;c++)
            if (IsInRect(MX,MY,CanvasX+(c*50),StartY+(r*42),45,38)) { HandleCalcAction(Buttons[r*4+c]); return TRUE; }
    }
    return TRUE;
}

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

BOOLEAN InputSysInfo(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) { return TRUE; }

STATIC CHAR16 EditorHlLower(CHAR16 Ch) {
    if (Ch >= L'A' && Ch <= L'Z')
        return Ch + (L'a' - L'A');
    return Ch;
}

STATIC BOOLEAN EditorHlHasExt(CONST CHAR16 *Ext) {
    UINTN NameLen = StrLen(gTextFileName);
    UINTN ExtLen = StrLen(Ext);

    if (NameLen < ExtLen) return FALSE;

    CONST CHAR16 *At = gTextFileName + NameLen - ExtLen;

    while (*At && *Ext) {
        if (EditorHlLower(*At) != EditorHlLower(*Ext))
            return FALSE;
        At++;
        Ext++;
    }

    return *At == L'\0' && *Ext == L'\0';
}

STATIC BOOLEAN EditorHlIsCodeFile(VOID) {
    STATIC CONST CHAR16 *Extensions[] = {
        L".c", L".h", L".cc", L".cpp", L".cxx",
        L".hpp", L".hxx", L".py", L".pyw",
        L".js", L".jsx", L".mjs", L".cjs",
        L".ts", L".tsx", L".java", L".cs",
        L".go", L".rs", L".php", L".rb",
        L".sh", L".bash", L".zsh", L".pl",
        L".json", L".xml", L".html", L".htm",
        L".svg", L".css", L".scss", L".sql",
        L".lua", L".yaml", L".yml", L".toml",
        L".ini", L".bat", L".cmd", L".ps1",
        L".swift", L".kt", L".kts", L".dart",
        L".m", L".mm", L".s", L".asm"
    };

    for (UINTN I = 0;
         I < sizeof(Extensions) / sizeof(Extensions[0]);
         I++) {
        if (EditorHlHasExt(Extensions[I]))
            return TRUE;
    }

    return FALSE;
}

STATIC BOOLEAN EditorHlHashComments(VOID) {
    return EditorHlHasExt(L".py")   ||
           EditorHlHasExt(L".pyw")  ||
           EditorHlHasExt(L".sh")   ||
           EditorHlHasExt(L".bash") ||
           EditorHlHasExt(L".zsh")  ||
           EditorHlHasExt(L".rb")   ||
           EditorHlHasExt(L".pl")   ||
           EditorHlHasExt(L".yaml") ||
           EditorHlHasExt(L".yml")  ||
           EditorHlHasExt(L".toml") ||
           EditorHlHasExt(L".ps1");
}

STATIC BOOLEAN EditorHlHtmlComments(VOID) {
    return EditorHlHasExt(L".html") ||
           EditorHlHasExt(L".htm")  ||
           EditorHlHasExt(L".xml")  ||
           EditorHlHasExt(L".svg");
}

STATIC BOOLEAN EditorHlDashComments(VOID) {
    return EditorHlHasExt(L".sql") ||
           EditorHlHasExt(L".lua");
}

STATIC BOOLEAN EditorHlWordEquals(
    CONST CHAR16 *Word,
    CONST CHAR16 *Expected
) {
    while (*Word && *Expected) {
        if (EditorHlLower(*Word) != EditorHlLower(*Expected))
            return FALSE;

        Word++;
        Expected++;
    }

    return *Word == L'\0' && *Expected == L'\0';
}

STATIC UINT32 EditorHlWordColor(CONST CHAR16 *Word) {
    STATIC CONST CHAR16 *Keywords[] = {
        L"if", L"else", L"elif", L"for", L"while", L"do",
        L"switch", L"case", L"default", L"break", L"continue",
        L"return", L"try", L"catch", L"finally", L"throw",
        L"raise", L"except", L"with", L"as", L"pass",
        L"match", L"async", L"await", L"yield", L"lambda",
        L"def", L"class", L"function", L"func", L"fn",
        L"let", L"var", L"const", L"static", L"extern",
        L"public", L"private", L"protected", L"internal",
        L"virtual", L"override", L"abstract", L"interface",
        L"implements", L"extends", L"new", L"delete",
        L"this", L"self", L"super", L"namespace",
        L"using", L"import", L"from", L"export",
        L"package", L"include", L"define", L"pragma",
        L"typedef", L"struct", L"enum", L"union",
        L"sizeof", L"typeof", L"instanceof", L"operator",
        L"template", L"typename", L"where", L"mod",
        L"pub", L"impl", L"use", L"crate", L"trait",
        L"select", L"insert", L"update", L"delete",
        L"create", L"drop", L"alter", L"table",
        L"from", L"join", L"where", L"group", L"order",
        L"by", L"having", L"limit", L"values",
		L"and", L"or", L"not", L"in", L"is",
        L"window", L"title", L"background", L"text",
        L"rect", L"circle"
    };

    STATIC CONST CHAR16 *Types[] = {
        L"void", L"bool", L"boolean", L"char", L"byte",
        L"short", L"int", L"long", L"float", L"double",
        L"string", L"object", L"decimal", L"number",
        L"integer", L"uint8_t", L"uint16_t", L"uint32_t",
        L"uint64_t", L"int8_t", L"int16_t", L"int32_t",
        L"int64_t", L"size_t", L"uintptr_t", L"usize",
        L"isize", L"str", L"string", L"String",
        L"EFI_STATUS", L"EFI_HANDLE", L"UINT8", L"UINT16",
        L"UINT32", L"UINT64", L"UINTN", L"INT32",
        L"BOOLEAN", L"CHAR16", L"VOID"
    };

    STATIC CONST CHAR16 *Constants[] = {
        L"true", L"false", L"null", L"nullptr",
        L"undefined", L"none", L"nil", L"nan",
        L"infinity", L"self"
    };

    STATIC CONST CHAR16 *Markup[] = {
        L"html", L"head", L"body", L"title", L"meta",
        L"link", L"script", L"style", L"div", L"span",
        L"p", L"a", L"img", L"input", L"button",
        L"form", L"label", L"ul", L"ol", L"li",
        L"table", L"tr", L"td", L"th", L"section",
        L"main", L"header", L"footer", L"nav",
        L"href", L"src", L"alt", L"class", L"id",
        L"type", L"value", L"name", L"width", L"height"
    };

    for (UINTN I = 0; I < sizeof(Keywords) / sizeof(Keywords[0]); I++) {
        if (EditorHlWordEquals(Word, Keywords[I]))
            return RGB(96,165,250);
    }

    for (UINTN I = 0; I < sizeof(Types) / sizeof(Types[0]); I++) {
        if (EditorHlWordEquals(Word, Types[I]))
            return RGB(74,222,128);
    }

    for (UINTN I = 0; I < sizeof(Constants) / sizeof(Constants[0]); I++) {
        if (EditorHlWordEquals(Word, Constants[I]))
            return RGB(244,114,182);
    }

    for (UINTN I = 0; I < sizeof(Markup) / sizeof(Markup[0]); I++) {
        if (EditorHlWordEquals(Word, Markup[I]))
            return RGB(96,165,250);
    }

    return COLOR_TEXT;
}

STATIC VOID EditorHlDrawChar(
    CHAR16 Ch,
    UINT32 Color,
    INT32 *X,
    INT32 *Y,
    INT32 CanvasX,
    INT32 CanvasW
) {
    DrawChar(Ch, *X, *Y, Color);
    *X += CharW();

    if (gWordWrap && *X + CharW() >= CanvasX + CanvasW - 12) {
        *X = CanvasX + 6;
        *Y += CharH() + 4;
    }
}

STATIC UINT32 SandboxRandom(VOID) {
    gSandboxRng = gSandboxRng * 1664525u + 1013904223u;
    return gSandboxRng;
}

STATIC BOOLEAN SandboxMove(
    INT32 X, INT32 Y,
    INT32 NX, INT32 NY,
    BOOLEAN SandCanDisplaceWater
) {
    if (NX < 0 || NX >= SANDBOX_GRID_W ||
        NY < 0 || NY >= SANDBOX_GRID_H)
        return FALSE;

    UINTN From = (UINTN)Y * SANDBOX_GRID_W + X;
    UINTN To = (UINTN)NY * SANDBOX_GRID_W + NX;
    UINT8 Material = gSandboxCells[From];
    UINT8 Destination = gSandboxCells[To];

    if (Destination == SB_EMPTY) {
        gSandboxCells[To] = Material;
        gSandboxCells[From] = SB_EMPTY;
        return TRUE;
    }

	if ((Material == SB_SAND || Material == SB_MUD) &&
		Destination == SB_WATER &&
		SandCanDisplaceWater) {
        gSandboxCells[To] = Material;
        gSandboxCells[From] = SB_WATER;
        return TRUE;
    }

    /* Water flowing into oil pushes oil upward. */
    if (Material == SB_WATER && Destination == SB_OIL) {
        gSandboxCells[To] = SB_WATER;
        gSandboxCells[From] = SB_OIL;
        return TRUE;
    }

    /* Acid can flow through water. */
    if (Material == SB_ACID && Destination == SB_WATER) {
        gSandboxCells[To] = SB_ACID;
        gSandboxCells[From] = SB_WATER;
        return TRUE;
    }

    return FALSE;
}

STATIC VOID UpdateSandbox(VOID) {
    if (!gSandboxWin.IsVisible ||
        gSandboxWin.IsMinimized ||
        gSandboxPaused)
        return;
    gSandboxTick++;

    /* Pass 1: sand, mud and water move downwards. */
    for (INT32 Y = SANDBOX_GRID_H - 2; Y >= 0; Y--) {
        INT32 StartX = (SandboxRandom() & 1U)
            ? 0 : SANDBOX_GRID_W - 1;
        INT32 Step = (StartX == 0) ? 1 : -1;

        for (INT32 N = 0; N < SANDBOX_GRID_W; N++) {
            INT32 X = StartX + N * Step;
            UINTN I = (UINTN)Y * SANDBOX_GRID_W + X;
            UINT8 Material = gSandboxCells[I];

            if (Material == SB_SAND || Material == SB_MUD) {
                if (Material == SB_MUD &&
                    SandboxRandom() % 3U != 0)
                    continue;

                if (SandboxMove(X, Y, X, Y + 1, TRUE))
                    continue;

                INT32 Side = (SandboxRandom() & 1U) ? -1 : 1;

                if (SandboxMove(X, Y, X + Side, Y + 1, TRUE))
                    continue;

                SandboxMove(X, Y, X - Side, Y + 1, TRUE);

            } else if (Material == SB_OIL) {
                /* Oil flows, but stays above water. */
                if (SandboxMove(X, Y, X, Y + 1, FALSE))
                    continue;

                INT32 Side =
                    (SandboxRandom() & 1U) ? -1 : 1;

                if (SandboxMove(
                        X, Y, X + Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(
                        X, Y, X - Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(X, Y, X + Side, Y, FALSE))
                    continue;

                SandboxMove(X, Y, X - Side, Y, FALSE);

            } else if (Material == SB_ACID) {
                /* Acid turns into water when it contacts water. */
                BOOLEAN Neutralized = FALSE;

                for (INT32 D = 0; D < 4; D++) {
                    INT32 NX = X;
                    INT32 NY = Y;

                    if (D == 0) NX--;
                    else if (D == 1) NX++;
                    else if (D == 2) NY--;
                    else NY++;

                    if (NX < 0 || NX >= SANDBOX_GRID_W ||
                        NY < 0 || NY >= SANDBOX_GRID_H)
                        continue;

                    if (gSandboxCells[NY * SANDBOX_GRID_W + NX]
                        == SB_WATER) {
                        gSandboxCells[I] = SB_WATER;
                        Neutralized = TRUE;
                        break;
                    }
                }

                if (Neutralized)
                    continue;

                /* Acid gradually dissolves nearby solids. */
                BOOLEAN Ate = FALSE;

                for (INT32 D = -1; D <= 1 && !Ate; D += 2) {
                    INT32 NX = X + D;

                    if (NX >= 0 && NX < SANDBOX_GRID_W) {
                        UINTN J =
                            (UINTN)Y * SANDBOX_GRID_W + NX;
                        UINT8 Other = gSandboxCells[J];

                        if ((Other == SB_SAND ||
                             Other == SB_MUD ||
                             Other == SB_WALL ||
                             Other == SB_STONE ||
                             Other == SB_ICE ||
                             Other == SB_PLANT) &&
                            SandboxRandom() % 3U == 0) {
                            gSandboxCells[J] = SB_EMPTY;
                            Ate = TRUE;
                        }
                    }

                    INT32 NY = Y + D;

                    if (!Ate &&
                        NY >= 0 && NY < SANDBOX_GRID_H) {
                        UINTN J =
                            (UINTN)NY * SANDBOX_GRID_W + X;
                        UINT8 Other = gSandboxCells[J];

                        if ((Other == SB_SAND ||
                             Other == SB_MUD ||
                             Other == SB_WALL ||
                             Other == SB_STONE ||
                             Other == SB_ICE ||
                             Other == SB_PLANT) &&
                            SandboxRandom() % 3U == 0) {
                            gSandboxCells[J] = SB_EMPTY;
                            Ate = TRUE;
                        }
                    }
                }

                if (SandboxMove(X, Y, X, Y + 1, FALSE))
                    continue;

                INT32 Side =
                    (SandboxRandom() & 1U) ? -1 : 1;

                if (SandboxMove(
                        X, Y, X + Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(
                        X, Y, X - Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(X, Y, X + Side, Y, FALSE))
                    continue;

                SandboxMove(X, Y, X - Side, Y, FALSE);

            } else if (Material == SB_LAVA) {
                /* Lava reacts with water and ice. */
                BOOLEAN Reacted = FALSE;

                for (INT32 D = -1;
                     D <= 1 && !Reacted;
                     D += 2) {
                    INT32 NX = X + D;

                    if (NX >= 0 && NX < SANDBOX_GRID_W) {
                        UINTN J =
                            (UINTN)Y * SANDBOX_GRID_W + NX;

                        if (gSandboxCells[J] == SB_WATER) {
                            gSandboxCells[J] = SB_STEAM;
                            gSandboxCells[I] = SB_STONE;
                            Reacted = TRUE;
                        } else if (gSandboxCells[J] == SB_ICE) {
                            gSandboxCells[J] = SB_WATER;
                            gSandboxCells[I] = SB_STONE;
                            Reacted = TRUE;
                        } else if (gSandboxCells[J] == SB_SAND) {
                            gSandboxCells[J] = SB_STONE;
                        } else if (gSandboxCells[J] == SB_PLANT ||
                                   gSandboxCells[J] == SB_OIL) {
                            gSandboxCells[J] = SB_FIRE;
                        }
                    }

                    INT32 NY = Y + D;

                    if (!Reacted &&
                        NY >= 0 && NY < SANDBOX_GRID_H) {
                        UINTN J =
                            (UINTN)NY * SANDBOX_GRID_W + X;

                        if (gSandboxCells[J] == SB_WATER) {
                            gSandboxCells[J] = SB_STEAM;
                            gSandboxCells[I] = SB_STONE;
                            Reacted = TRUE;
                        } else if (gSandboxCells[J] == SB_ICE) {
                            gSandboxCells[J] = SB_WATER;
                            gSandboxCells[I] = SB_STONE;
                            Reacted = TRUE;
                        } else if (gSandboxCells[J] == SB_SAND) {
                            gSandboxCells[J] = SB_STONE;
                        } else if (gSandboxCells[J] == SB_PLANT ||
                                   gSandboxCells[J] == SB_OIL) {
                            gSandboxCells[J] = SB_FIRE;
                        }
                    }
                }

                if (Reacted)
                    continue;

                /* Lava is slower than ordinary liquids. */
                if (SandboxRandom() % 3U != 0)
                    continue;

                if (SandboxMove(X, Y, X, Y + 1, FALSE))
                    continue;

                INT32 Side =
                    (SandboxRandom() & 1U) ? -1 : 1;

                if (SandboxMove(
                        X, Y, X + Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(
                        X, Y, X - Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(X, Y, X + Side, Y, FALSE))
                    continue;

                SandboxMove(X, Y, X - Side, Y, FALSE);

            } else if (Material == SB_PLANT) {
                /* Plants grow when water is nearby. */
                BOOLEAN HasWater = FALSE;

                for (INT32 D = -1; D <= 1; D += 2) {
                    INT32 NX = X + D;
                    INT32 NY = Y + D;

                    if (NX >= 0 && NX < SANDBOX_GRID_W &&
                        gSandboxCells[Y * SANDBOX_GRID_W + NX]
                            == SB_WATER)
                        HasWater = TRUE;

                    if (NY >= 0 && NY < SANDBOX_GRID_H &&
                        gSandboxCells[NY * SANDBOX_GRID_W + X]
                            == SB_WATER)
                        HasWater = TRUE;
                }

                if (HasWater &&
                    SandboxRandom() % 8U == 0) {
                    if (Y > 0 &&
                        gSandboxCells[I - SANDBOX_GRID_W]
                            == SB_EMPTY) {
                        gSandboxCells[I - SANDBOX_GRID_W]
                            = SB_PLANT;
                    } else {
                        INT32 Side =
                            (SandboxRandom() & 1U) ? -1 : 1;
                        INT32 NX = X + Side;

                        if (NX >= 0 && NX < SANDBOX_GRID_W &&
                            gSandboxCells[
                                Y * SANDBOX_GRID_W + NX
                            ] == SB_EMPTY) {
                            gSandboxCells[
                                Y * SANDBOX_GRID_W + NX
                            ] = SB_PLANT;
                        }
                    }
                }

            } else if (Material == SB_ICE) {
                /* Ice melts near heat and floats upward in water. */
                BOOLEAN Melted = FALSE;

                for (INT32 D = 0; D < 4; D++) {
                    INT32 NX = X;
                    INT32 NY = Y;

                    if (D == 0) NX--;
                    else if (D == 1) NX++;
                    else if (D == 2) NY--;
                    else NY++;

                    if (NX < 0 || NX >= SANDBOX_GRID_W ||
                        NY < 0 || NY >= SANDBOX_GRID_H)
                        continue;

                    UINT8 Other =
                        gSandboxCells[NY * SANDBOX_GRID_W + NX];

                    if (Other == SB_FIRE || Other == SB_LAVA) {
                        gSandboxCells[I] = SB_WATER;
                        Melted = TRUE;
                        break;
                    }
                }

                if (Melted)
                    continue;

                /* Ice placed under water rises to the surface. */
                if (Y > 0 &&
                    gSandboxCells[I - SANDBOX_GRID_W] == SB_WATER) {
                    gSandboxCells[I - SANDBOX_GRID_W] = SB_ICE;
                    gSandboxCells[I] = SB_WATER;
                    continue;
                }

                /* Ice falls through empty space. */
                if (SandboxMove(X, Y, X, Y + 1, FALSE))
                    continue;

                INT32 Side =
                    (SandboxRandom() & 1U) ? -1 : 1;

                if (SandboxMove(
                        X, Y, X + Side, Y + 1, FALSE))
                    continue;

                SandboxMove(
                    X, Y, X - Side, Y + 1, FALSE
                );

            } else if (Material == SB_WATER) {
                BOOLEAN Reacted = FALSE;

                for (INT32 D = -1; D <= 1 && !Reacted; D += 2) {
                    INT32 NY = Y + D;

                    if (NY >= 0 && NY < SANDBOX_GRID_H &&
                        gSandboxCells[NY * SANDBOX_GRID_W + X] == SB_FIRE) {
                        gSandboxCells[NY * SANDBOX_GRID_W + X] = SB_SMOKE;
                        gSandboxCells[I] = SB_STEAM;
                        Reacted = TRUE;
                    }

                    INT32 NX = X + D;

                    if (!Reacted && NX >= 0 && NX < SANDBOX_GRID_W &&
                        gSandboxCells[Y * SANDBOX_GRID_W + NX] == SB_FIRE) {
                        gSandboxCells[Y * SANDBOX_GRID_W + NX] = SB_SMOKE;
                        gSandboxCells[I] = SB_STEAM;
                        Reacted = TRUE;
                    }
                }

                if (Reacted)
                    continue;

                if (SandboxMove(X, Y, X, Y + 1, FALSE))
                    continue;

                INT32 Side = (SandboxRandom() & 1U) ? -1 : 1;

                if (SandboxMove(X, Y, X + Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(X, Y, X - Side, Y + 1, FALSE))
                    continue;

                if (SandboxMove(X, Y, X + Side, Y, FALSE))
                    continue;

                SandboxMove(X, Y, X - Side, Y, FALSE);
            }
        }
    }

    /* Pass 2: fire, steam and smoke rise or dissipate. */
    for (INT32 Y = 0; Y < SANDBOX_GRID_H; Y++) {
        for (INT32 X = 0; X < SANDBOX_GRID_W; X++) {
            UINTN I = (UINTN)Y * SANDBOX_GRID_W + X;
            UINT8 Material = gSandboxCells[I];

            if (Material == SB_FIRE) {
                /* Find water next to the flame. */
                BOOLEAN Reacted = FALSE;

                for (INT32 D = -1; D <= 1 && !Reacted; D += 2) {
                    INT32 NY = Y + D;

                    if (NY >= 0 && NY < SANDBOX_GRID_H &&
                        gSandboxCells[NY * SANDBOX_GRID_W + X] == SB_WATER) {
                        gSandboxCells[NY * SANDBOX_GRID_W + X] = SB_STEAM;
                        gSandboxCells[I] = SB_SMOKE;
                        Reacted = TRUE;
                    }

                    INT32 NX = X + D;

                    if (!Reacted && NX >= 0 && NX < SANDBOX_GRID_W &&
                        gSandboxCells[Y * SANDBOX_GRID_W + NX] == SB_WATER) {
                        gSandboxCells[Y * SANDBOX_GRID_W + NX] = SB_STEAM;
                        gSandboxCells[I] = SB_SMOKE;
                        Reacted = TRUE;
                    }
                }

                if (!Reacted) {
                    for (INT32 D = -1; D <= 1; D += 2) {
                        INT32 NX = X + D;

                        if (NX >= 0 && NX < SANDBOX_GRID_W) {
                            UINTN J =
                                (UINTN)Y * SANDBOX_GRID_W + NX;

                            if (gSandboxCells[J] == SB_OIL)
                                gSandboxCells[J] = SB_FIRE;
                            else if (gSandboxCells[J] == SB_PLANT)
                                gSandboxCells[J] = SB_SMOKE;
                            else if (gSandboxCells[J] == SB_ICE)
                                gSandboxCells[J] = SB_WATER;
                        }

                        INT32 NY = Y + D;

                        if (NY >= 0 && NY < SANDBOX_GRID_H) {
                            UINTN J =
                                (UINTN)NY * SANDBOX_GRID_W + X;

                            if (gSandboxCells[J] == SB_OIL)
                                gSandboxCells[J] = SB_FIRE;
                            else if (gSandboxCells[J] == SB_PLANT)
                                gSandboxCells[J] = SB_SMOKE;
                            else if (gSandboxCells[J] == SB_ICE)
                                gSandboxCells[J] = SB_WATER;
                        }
                    }
                }

                if (Reacted)
                    continue;

                if (SandboxRandom() % 10U == 0) {
                    gSandboxCells[I] = SB_SMOKE;
                    continue;
                }

                if (Y == 0)
                    continue;

                UINTN Above = I - SANDBOX_GRID_W;

                if (gSandboxCells[Above] == SB_EMPTY) {
                    gSandboxCells[Above] = SB_FIRE;
                    gSandboxCells[I] = SB_EMPTY;
                }

            } else if (Material == SB_STEAM ||
                       Material == SB_SMOKE) {
                UINT32 Chance = SandboxRandom();

                if ((Material == SB_STEAM && Chance % 24U == 0) ||
                    (Material == SB_SMOKE && Chance % 12U == 0)) {
                    gSandboxCells[I] = SB_EMPTY;
                    continue;
                }

                if (Y > 0) {
                    UINTN Above = I - SANDBOX_GRID_W;

                    if (gSandboxCells[Above] == SB_EMPTY) {
                        gSandboxCells[Above] = Material;
                        gSandboxCells[I] = SB_EMPTY;
                        continue;
                    }
                }

                INT32 Side = (Chance & 1U) ? -1 : 1;
                INT32 NX = X + Side;

                if (NX >= 0 && NX < SANDBOX_GRID_W &&
                    gSandboxCells[Y * SANDBOX_GRID_W + NX] == SB_EMPTY) {
                    gSandboxCells[Y * SANDBOX_GRID_W + NX] = Material;
                    gSandboxCells[I] = SB_EMPTY;
                }
            }
            /* Stone and walls remain stationary. */
        }
    }
}

VOID RenderSandbox(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 12;
    INT32 Row1 = Win->Y + 34;
    INT32 Row2 = Win->Y + 61;
    INT32 Row3 = Win->Y + 88;

    DrawButton(X, Row1, 76, 22, L"Sand",
        gSandboxTool == SB_SAND ? RGB(180,130,35) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+82, Row1, 76, 22, L"Water",
        gSandboxTool == SB_WATER ? RGB(25,100,210) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+164, Row1, 76, 22, L"Mud",
        gSandboxTool == SB_MUD ? RGB(121,85,58) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+246, Row1, 76, 22, L"Stone",
        gSandboxTool == SB_STONE ? RGB(100,110,125) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+328, Row1, 76, 22, L"Wall",
        gSandboxTool == SB_WALL ? RGB(135,145,160) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+410, Row1, 76, 22, L"Fire",
        gSandboxTool == SB_FIRE ? RGB(220,75,20) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+492, Row1, 76, 22, L"Steam",
        gSandboxTool == SB_STEAM ? RGB(40,145,175) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);

    DrawButton(X, Row2, 76, 22, L"Erase",
        gSandboxTool == SB_EMPTY ? RGB(190,65,100) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
        DrawButton(X, Row3, 76, 22, L"Oil",
        gSandboxTool == SB_OIL ? RGB(100,90,30) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);

    DrawButton(X+82, Row3, 76, 22, L"Lava",
        gSandboxTool == SB_LAVA ? RGB(230,60,10) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);

    DrawButton(X+164, Row3, 76, 22, L"Acid",
        gSandboxTool == SB_ACID ? RGB(100,190,20) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);

    DrawButton(X+246, Row3, 76, 22, L"Ice",
        gSandboxTool == SB_ICE ? RGB(100,195,235) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);

    DrawButton(X+328, Row3, 76, 22, L"Plant",
        gSandboxTool == SB_PLANT ? RGB(30,150,55) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+82, Row2, 76, 22, L"Smoke",
        gSandboxTool == SB_SMOKE ? RGB(100,105,115) : COLOR_TOOLBAR_BG,
        COLOR_TEXT);
    DrawButton(X+164, Row2, 96, 22,
        gSandboxPaused ? L"Resume" : L"Pause",
        NEW_APP_COLOR_ACCENT, COLOR_TEXT);
    DrawButton(X+266, Row2, 76, 22, L"Clear",
        COLOR_BTN_CLOSE, COLOR_TEXT);

    INT32 CanvasX = Win->X + 12;
    INT32 CanvasY = Win->Y + 117;
    INT32 CanvasW = SANDBOX_GRID_W * SANDBOX_CELL_SIZE;
    INT32 CanvasH = SANDBOX_GRID_H * SANDBOX_CELL_SIZE;

    DrawRect(CanvasX, CanvasY, CanvasW, CanvasH,
        RGB(8, 12, 20));

    for (INT32 Y = 0; Y < SANDBOX_GRID_H; Y++) {
        for (INT32 XX = 0; XX < SANDBOX_GRID_W; XX++) {
            UINT8 Material =
                gSandboxCells[Y * SANDBOX_GRID_W + XX];

            UINT32 Color;

            if (Material == SB_EMPTY)
                continue;
            else if (Material == SB_SAND)
                Color = RGB(238,208,105);
            else if (Material == SB_WATER)
                Color = RGB(48,145,250);
            else if (Material == SB_MUD)
                Color = RGB(121,85,58);
            else if (Material == SB_STONE)
                Color = RGB(110,120,135);
            else if (Material == SB_WALL)
                Color = RGB(175,185,195);
            else if (Material == SB_FIRE)
                Color = ((XX + Y + (gFrameCounter >> 1)) & 1U)
                    ? RGB(255,80,20) : RGB(255,205,35);
            else if (Material == SB_STEAM)
                Color = RGB(125,225,240);
            else if (Material == SB_SMOKE)
                Color = RGB(105,110,120);
            else if (Material == SB_OIL)
                Color = RGB(80,85,35);
            else if (Material == SB_LAVA)
                Color = ((XX + Y + (gFrameCounter >> 1)) & 1U)
                    ? RGB(255,35,0) : RGB(255,135,10);
            else if (Material == SB_ACID)
                Color = RGB(120,255,30);
            else if (Material == SB_ICE)
                Color = RGB(155,235,255);
            else if (Material == SB_PLANT)
                Color = RGB(45,205,75);
            else
                continue;

            DrawRect(
                CanvasX + XX * SANDBOX_CELL_SIZE,
                CanvasY + Y * SANDBOX_CELL_SIZE,
                SANDBOX_CELL_SIZE,
                SANDBOX_CELL_SIZE,
                Color
            );
        }
    }

    DrawString(
        gSandboxPaused
            ? L"PAUSED - choose a material and draw"
            : L"Click and drag to pour particles",
        X, Win->Y + Win->H - 24, COLOR_TEXT
    );
}

BOOLEAN InputSandbox(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    INT32 X = Win->X + 12;
    INT32 Row1 = Win->Y + 34;
    INT32 Row2 = Win->Y + 61;
    INT32 Row3 = Win->Y + 88;

    if (Click && !Last) {
        if (IsInRect(MX,MY,X,Row1,76,22)) {
            gSandboxTool = SB_SAND;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+82,Row1,76,22)) {
            gSandboxTool = SB_WATER;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+164,Row1,76,22)) {
            gSandboxTool = SB_MUD;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+246,Row1,76,22)) {
            gSandboxTool = SB_STONE;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+328,Row1,76,22)) {
            gSandboxTool = SB_WALL;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+410,Row1,76,22)) {
            gSandboxTool = SB_FIRE;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+492,Row1,76,22)) {
            gSandboxTool = SB_STEAM;
            return TRUE;
        }

        if (IsInRect(MX,MY,X,Row2,76,22)) {
            gSandboxTool = SB_EMPTY;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+82,Row2,76,22)) {
            gSandboxTool = SB_SMOKE;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+164,Row2,96,22)) {
            gSandboxPaused = !gSandboxPaused;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+266,Row2,76,22)) {
            for (UINTN I = 0;
                 I < SANDBOX_GRID_W * SANDBOX_GRID_H;
                 I++) {
                gSandboxCells[I] = SB_EMPTY;
            }
            return TRUE;
        }
        
                if (IsInRect(MX, MY, X, Row3, 76, 22)) {
            gSandboxTool = SB_OIL;
            return TRUE;
        }

        if (IsInRect(MX, MY, X+82, Row3, 76, 22)) {
            gSandboxTool = SB_LAVA;
            return TRUE;
        }

        if (IsInRect(MX, MY, X+164, Row3, 76, 22)) {
            gSandboxTool = SB_ACID;
            return TRUE;
        }

        if (IsInRect(MX, MY, X+246, Row3, 76, 22)) {
            gSandboxTool = SB_ICE;
            return TRUE;
        }

        if (IsInRect(MX, MY, X+328, Row3, 76, 22)) {
            gSandboxTool = SB_PLANT;
            return TRUE;
        }
    }
    
    INT32 CanvasX = Win->X + 12;
    INT32 CanvasY = Win->Y + 117;
    INT32 CanvasW = SANDBOX_GRID_W * SANDBOX_CELL_SIZE;
    INT32 CanvasH = SANDBOX_GRID_H * SANDBOX_CELL_SIZE;

    if (Click &&
        IsInRect(MX, MY, CanvasX, CanvasY, CanvasW, CanvasH)) {
        INT32 GX = (MX - CanvasX) / SANDBOX_CELL_SIZE;
        INT32 GY = (MY - CanvasY) / SANDBOX_CELL_SIZE;

        /* Small brush: 2 by 2 simulation cells. */
        for (INT32 DY = 0; DY < 2; DY++) {
            for (INT32 DX = 0; DX < 2; DX++) {
                INT32 PX = GX + DX;
                INT32 PY = GY + DY;

                if (PX < 0 || PX >= SANDBOX_GRID_W ||
                    PY < 0 || PY >= SANDBOX_GRID_H)
                    continue;

                gSandboxCells[PY * SANDBOX_GRID_W + PX] =
                    gSandboxTool;
            }
        }

        return TRUE;
    }

    return TRUE;
}

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
            if (MiniLineX>=MiniX+MinimapW-6) { MiniY+=3; MiniLineX=MiniX+4; }
        }
    }
    INT32 CurX = CanvasX + 6;
    INT32 CurY = CanvasY + 6;

    BOOLEAN CodeMode = EditorHlIsCodeFile();
    BOOLEAN HashComments = EditorHlHashComments();
    BOOLEAN HtmlComments = EditorHlHtmlComments();
    BOOLEAN DashComments = EditorHlDashComments();

    BOOLEAN InLineComment = FALSE;
    BOOLEAN InBlockComment = FALSE;
    BOOLEAN InHtmlComment = FALSE;
    BOOLEAN Escaped = FALSE;
    CHAR16 StringQuote = 0;

    for (UINTN I = 0; I < gTextLen; ) {
        CHAR16 Ch = gTextBuffer[I];

        if (Ch == L'\n') {
            CurX = CanvasX + 6;
            CurY += CharH() + 4;
            I++;

            InLineComment = FALSE;

            if (StringQuote != 0 && !Escaped)
                StringQuote = 0;

            Escaped = FALSE;

            if (CurY + CharH() + 4 >= CanvasY + CanvasH)
                break;

            continue;
        }

        if (!CodeMode) {
            EditorHlDrawChar(
                Ch, COLOR_TEXT, &CurX, &CurY, CanvasX, CanvasW
            );
            I++;
            continue;
        }

        /* A line comment continues up to the next newline. */
        if (InLineComment) {
            EditorHlDrawChar(
                Ch, RGB(100,116,139),
                &CurX, &CurY, CanvasX, CanvasW
            );
            I++;
            continue;
        }

        /* HTML/XML comments: <!-- ... --> */
        if (InHtmlComment) {
            if (Ch == L'-' && I + 2 < gTextLen &&
                gTextBuffer[I + 1] == L'-' &&
                gTextBuffer[I + 2] == L'>') {

                for (UINTN K = 0; K < 3; K++) {
                    EditorHlDrawChar(
                        gTextBuffer[I + K], RGB(100,116,139),
                        &CurX, &CurY, CanvasX, CanvasW
                    );
                }

                I += 3;
                InHtmlComment = FALSE;
                continue;
            }

            EditorHlDrawChar(
                Ch, RGB(100,116,139),
                &CurX, &CurY, CanvasX, CanvasW
            );
            I++;
            continue;
        }

		/* Handle the contents of a C-style block comment. */
        if (InBlockComment) {
            if (Ch == L'*' && I + 1 < gTextLen &&
                gTextBuffer[I + 1] == L'/') {

                EditorHlDrawChar(
                    L'*', RGB(100,116,139),
                    &CurX, &CurY, CanvasX, CanvasW
                );
                EditorHlDrawChar(
                    L'/', RGB(100,116,139),
                    &CurX, &CurY, CanvasX, CanvasW
                );

                I += 2;
                InBlockComment = FALSE;
                continue;
            }

            EditorHlDrawChar(
                Ch, RGB(100,116,139),
                &CurX, &CurY, CanvasX, CanvasW
            );
            I++;
            continue;
        }

        /* Strings use double quotes, single quotes or backticks. */
        if (StringQuote != 0) {
            EditorHlDrawChar(
                Ch, RGB(234,179,8),
                &CurX, &CurY, CanvasX, CanvasW
            );

            if (Escaped) {
                Escaped = FALSE;
            } else if (Ch == L'\\') {
                Escaped = TRUE;
            } else if (Ch == StringQuote) {
                StringQuote = 0;
            }

            I++;
            continue;
        }

        /* Start HTML/XML comment. */
        if (HtmlComments && Ch == L'<' &&
            I + 3 < gTextLen &&
            gTextBuffer[I + 1] == L'!' &&
            gTextBuffer[I + 2] == L'-' &&
            gTextBuffer[I + 3] == L'-') {

            for (UINTN K = 0; K < 4; K++) {
                EditorHlDrawChar(
                    gTextBuffer[I + K], RGB(100,116,139),
                    &CurX, &CurY, CanvasX, CanvasW
                );
            }

            I += 4;
            InHtmlComment = TRUE;
            continue;
        }

		/* Detect the start of a line comment or block comment. */
        if (Ch == L'/' && I + 1 < gTextLen) {
            if (gTextBuffer[I + 1] == L'/') {
                InLineComment = TRUE;
                continue;
            }

            if (gTextBuffer[I + 1] == L'*') {
                EditorHlDrawChar(
                    L'/', RGB(100,116,139),
                    &CurX, &CurY, CanvasX, CanvasW
                );
                EditorHlDrawChar(
                    L'*', RGB(100,116,139),
                    &CurX, &CurY, CanvasX, CanvasW
                );

                I += 2;
                InBlockComment = TRUE;
                continue;
            }
        }

        /* Python/shell-style # comments. */
        if (Ch == L'#' && HashComments) {
            InLineComment = TRUE;
            continue;
        }

        /* SQL/Lua-style -- comments. */
        if (Ch == L'-' && I + 1 < gTextLen &&
            gTextBuffer[I + 1] == L'-' && DashComments) {
            InLineComment = TRUE;
            continue;
        }

        if (Ch == L'"' || Ch == L'\'' || Ch == L'`') {
            StringQuote = Ch;
            Escaped = FALSE;

            EditorHlDrawChar(
                Ch, RGB(234,179,8),
                &CurX, &CurY, CanvasX, CanvasW
            );
            I++;
            continue;
        }

        /* Highlight numeric tokens. */
        if (Ch >= L'0' && Ch <= L'9') {
            UINTN Start = I;

            while (I < gTextLen) {
                Ch = gTextBuffer[I];

                BOOLEAN IsNumberChar =
                    (Ch >= L'0' && Ch <= L'9') ||
                    (Ch >= L'a' && Ch <= L'z') ||
                    (Ch >= L'A' && Ch <= L'Z') ||
                    Ch == L'.' || Ch == L'_';

                if (!IsNumberChar)
                    break;

                I++;
            }

            for (UINTN K = Start; K < I; K++) {
                EditorHlDrawChar(
                    gTextBuffer[K], RGB(56,189,248),
                    &CurX, &CurY, CanvasX, CanvasW
                );
            }

            continue;
        }

        /* Highlight identifier/keyword tokens. */
        BOOLEAN IsWordStart =
            (Ch >= L'a' && Ch <= L'z') ||
            (Ch >= L'A' && Ch <= L'Z') ||
            Ch == L'_';

        if (IsWordStart) {
            UINTN Start = I;
            UINTN WordLen = 0;
            CHAR16 Word[96];

            while (I < gTextLen) {
                Ch = gTextBuffer[I];

                BOOLEAN IsWordChar =
                    (Ch >= L'a' && Ch <= L'z') ||
                    (Ch >= L'A' && Ch <= L'Z') ||
                    (Ch >= L'0' && Ch <= L'9') ||
                    Ch == L'_';

                if (!IsWordChar)
                    break;

                if (WordLen < 95)
                    Word[WordLen++] = Ch;

                I++;
            }

            Word[WordLen] = L'\0';

            UINT32 WordColor = EditorHlWordColor(Word);

            /* Do not color truncated, overly long identifiers. */
            if (I - Start > 95)
                WordColor = COLOR_TEXT;

            for (UINTN K = Start; K < I; K++) {
                EditorHlDrawChar(
                    gTextBuffer[K], WordColor,
                    &CurX, &CurY, CanvasX, CanvasW
                );
            }

            continue;
        }

        EditorHlDrawChar(
            Ch, COLOR_TEXT, &CurX, &CurY, CanvasX, CanvasW
        );
        I++;
    }

    DrawRect(CurX, CurY, CharW(), CharH() + 2, COLOR_CURSOR);
    if (gEditorMenuState==EDITOR_MENU_FILE) {
        DrawRect(Win->X+5,WinY+MenuH,100,45,COLOR_MENU_BG);
        DrawString(L"Save As...",Win->X+10,WinY+MenuH+6,COLOR_TEXT);
        DrawString(L"Open...",Win->X+10,WinY+MenuH+24,COLOR_TEXT);
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
        else gEditorMenuState=EDITOR_MENU_NONE;
        return TRUE;
    }
    if (Click && !Last && gEditorMenuState!=EDITOR_MENU_NONE) {
        if (gEditorMenuState==EDITOR_MENU_FILE) {
            if (IsInRect(MX,MY,Win->X+5,WinY+MenuH,100,22)) {
                FileDialogOpen(FILEDLG_SAVE, FD_P_EDITOR_SAVE);
            } else if (IsInRect(MX,MY,Win->X+5,WinY+MenuH+22,100,22)) {
                FileDialogOpen(FILEDLG_OPEN, FD_P_EDITOR_LOAD);
            }
        } else if (gEditorMenuState==EDITOR_MENU_EDIT) {
            if (IsInRect(MX,MY,Win->X+55,WinY+MenuH,130,28)) {
                gFindWin.IsVisible=TRUE; gFindWin.IsMinimized=FALSE;
                BringToFront(&gFindWin);
            }
        } else if (gEditorMenuState==EDITOR_MENU_VIEW) {
            if (IsInRect(MX,MY,Win->X+105,WinY+MenuH,120,20)) gWordWrap=!gWordWrap;
            else if (IsInRect(MX,MY,Win->X+105,WinY+MenuH+20,120,24)) gShowMinimap=!gShowMinimap;
        }
        gEditorMenuState=EDITOR_MENU_NONE;
        return TRUE;
    }
    return FALSE;
}

VOID RotateCanvasCW(VOID) {
    UINT32 Temp[PAINT_W*PAINT_H];
    for (INT32 y=0;y<PAINT_H;y++) for (INT32 x=0;x<PAINT_W;x++)
        Temp[x*PAINT_H+(PAINT_H-1-y)]=gPaintCanvas[y*PAINT_W+x];
    for (INT32 y=0;y<PAINT_H;y++) for (INT32 x=0;x<PAINT_W;x++)
        gPaintCanvas[y*PAINT_W+x]=Temp[y*PAINT_W+x];
}

VOID DrawCanvasShape(INT32 Cx,INT32 Cy,PAINT_TOOL Tool) {
    if (Tool==TOOL_SQUARE) {
        for (INT32 dy=-10;dy<=10;dy++) for (INT32 dx=-10;dx<=10;dx++) {
            if (!(dx==-10 || dx==10 || dy==-10 || dy==10)) continue;
            INT32 px=Cx+dx,py=Cy+dy;
            if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H) gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
        }
    } else if (Tool==TOOL_CIRCLE) {
        for (INT32 dy=-10;dy<=10;dy++) for (INT32 dx=-10;dx<=10;dx++) {
            INT32 d2 = dx*dx + dy*dy;
            if (d2 < 81 || d2 > 100) continue;
            INT32 px=Cx+dx,py=Cy+dy;
            if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H) gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
        }
    } else if (Tool==TOOL_TRIANGLE) {
        for (INT32 dy=0;dy<=16;dy++) {
            INT32 HalfW=dy/2;
            for (INT32 dx=-HalfW;dx<=HalfW;dx++) {
                if (!(dx==-HalfW || dx==HalfW || dy==16)) continue;
                INT32 px=Cx+dx,py=Cy-8+dy;
                if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H) gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
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
    for (INT32 py=0;py<PAINT_H;py++) for (INT32 px=0;px<PAINT_W;px++) {
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
            FileDialogOpen(FILEDLG_SAVE, FD_P_PAINT_SAVE);
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
            for (INT32 dy=0;dy<BrushSize;dy++) for (INT32 dx=0;dx<BrushSize;dx++) {
                INT32 px=StartX+dx,py=StartY+dy;
                if (px>=0&&px<PAINT_W&&py>=0&&py<PAINT_H) gPaintCanvas[py*PAINT_W+px]=gCurrentColor;
            }
        } else if (gCurrentTool==TOOL_SQUARE||gCurrentTool==TOOL_CIRCLE||gCurrentTool==TOOL_TRIANGLE) {
            if (!Last) DrawCanvasShape(CanvasTargetX,CanvasTargetY,gCurrentTool);
        } else if (gCurrentTool==TOOL_TEXT&&!Last) {
            gPaintTypingText=TRUE;
            gPaintTextTargetX=CanvasTargetX; gPaintTextTargetY=CanvasTargetY;
            gPaintTextBuf[0]=L'\0'; gPaintTextLen=0;
        } else if (gCurrentTool==TOOL_PAN||gCurrentTool==TOOL_MOVE) {
            if (!Last) { gPanLastX=MX; gPanLastY=MY; }
            else { gPanOffsetX+=(MX-gPanLastX); gPanOffsetY+=(MY-gPanLastY); gPanLastX=MX; gPanLastY=MY; }
        }
    }
    return TRUE;
}

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

VOID RenderAnimEdit(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    DrawRect(CanvasX,CanvasY,Win->W-16,26,COLOR_TOOLBAR_BG);
    UINT32 Palette[]={RGB(255,0,0),RGB(0,255,0),RGB(0,0,255),RGB(255,255,0),RGB(255,255,255),RGB(0,0,0)};
    for (INT32 i=0;i<6;i++) DrawRect(CanvasX+6+(i*25),CanvasY+3,20,20,Palette[i]);
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
    for (INT32 py=0;py<ANIM_H;py++) for (INT32 px=0;px<ANIM_W;px++)
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
        if (IsInRect(MX,MY,CanvasX+5,ControlY+2,24,20)&&gEditFrameIdx>0) gEditFrameIdx--;
        else if (IsInRect(MX,MY,CanvasX+140,ControlY+2,24,20)&&gEditFrameIdx<gAnimFrameCount-1) gEditFrameIdx++;
        else if (IsInRect(MX,MY,CanvasX+175,ControlY+2,55,20)&&gAnimFrameCount<MAX_ANIM_FRAMES) gEditFrameIdx=++gAnimFrameCount-1;
        else if (IsInRect(MX,MY,CanvasX+238,ControlY+2,65,20)&&gEditFrameIdx>0) gBS->CopyMem(gAnimFrames[gEditFrameIdx],gAnimFrames[gEditFrameIdx-1],sizeof(gAnimFrames[0]));
        else if (IsInRect(MX,MY,CanvasX+310,ControlY+2,50,20))
            for (UINT32 p=0;p<ANIM_W*ANIM_H;p++) gAnimFrames[gEditFrameIdx][p]=RGB(15,15,15);
    }
    INT32 FrameX=CanvasX+80,FrameY=ControlY+32;
    if (Click&&IsInRect(MX,MY,FrameX,FrameY,ANIM_W*2,ANIM_H*2)) {
        INT32 px=(MX-FrameX)/2,py=(MY-FrameY)/2;
        if (px>=0&&px<ANIM_W&&py>=0&&py<ANIM_H) gAnimFrames[gEditFrameIdx][py*ANIM_W+px]=gAnimColor;
    }
    return TRUE;
}

VOID RenderAnimView(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    DrawRect(CanvasX,CanvasY,Win->W-16,26,COLOR_TOOLBAR_BG);
    DrawButton(CanvasX+6,CanvasY+3,60,20,gAnimPlaying?L"Pause":L" Play",gAnimPlaying?COLOR_BTN_CLOSE:COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+75,CanvasY+3,24,20,L"<",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+105,CanvasY+3,24,20,L">",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(CanvasX+140,CanvasY+3,55,20,L"Slower",COLOR_WIN_BORDER,COLOR_TEXT);
    DrawButton(CanvasX+200,CanvasY+3,55,20,L"Faster",COLOR_WIN_BORDER,COLOR_TEXT);
    INT32 ViewX=CanvasX+30,ViewY=CanvasY+36;
    for (INT32 py=0;py<ANIM_H;py++) for (INT32 px=0;px<ANIM_W;px++)
        DrawRect(ViewX+(px*2),ViewY+(py*2),2,2,gAnimFrames[gViewFrameIdx][py*ANIM_W+px]);
    CHAR16 ViewStr[32];
    UnicodeSPrint(ViewStr,sizeof(ViewStr),L"Playing Frame: %d / %d",gViewFrameIdx+1,gAnimFrameCount);
    DrawString(ViewStr,CanvasX+40,ViewY+(ANIM_H*2)+8,COLOR_TEXT);
}

BOOLEAN InputAnimView(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 CanvasX=Win->X+8,CanvasY=Win->Y+34;
    if (Click&&!Last&&IsInRect(MX,MY,CanvasX,CanvasY+3,Win->W-16,20)) {
        if (IsInRect(MX,MY,CanvasX+6,CanvasY+3,60,20)) gAnimPlaying=!gAnimPlaying;
        else if (IsInRect(MX,MY,CanvasX+75,CanvasY+3,24,20)) gViewFrameIdx=(gViewFrameIdx>0)?gViewFrameIdx-1:gAnimFrameCount-1;
        else if (IsInRect(MX,MY,CanvasX+105,CanvasY+3,24,20)) gViewFrameIdx=(gViewFrameIdx+1)%gAnimFrameCount;
        else if (IsInRect(MX,MY,CanvasX+140,CanvasY+3,55,20)&&gAnimSpeedDelay<30) gAnimSpeedDelay+=2;
        else if (IsInRect(MX,MY,CanvasX+200,CanvasY+3,55,20)&&gAnimSpeedDelay>2) gAnimSpeedDelay-=2;
    }
    return TRUE;
}

VOID RenderFileManager(UEFI_WINDOW *Win) {
    ResizeFileManagerToFit(Win);
    DrawWindowTitleBar(Win);
    INT32 CanvasX = Win->X + 8;
    INT32 CanvasY = Win->Y + 34;
    DrawRect(CanvasX,CanvasY,Win->W-16,Win->H-42,COLOR_CANVAS);
    DrawString(L"Files on EFI:",CanvasX+10,CanvasY+10,COLOR_TEXT);
    for (UINTN i = 0, ItemY = CanvasY + 30; i < gFileCount; i++, ItemY += 18) {
        UINT32 ItemColor = (gSelectedFileIndex == (INT32)i) ? COLOR_TITLE_ACTIVE : COLOR_TOOLBAR_BG;
        DrawRect(CanvasX+10,ItemY,220,17,ItemColor);
        DrawString(gFileList[i],CanvasX+15,ItemY+4,COLOR_TEXT);
    }
    DrawButton(CanvasX+250,CanvasY+30,80,25,L"Open",COLOR_TITLE_ACTIVE,COLOR_TEXT);
}

BOOLEAN InputFileManager(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    ResizeFileManagerToFit(Win);
    INT32 CanvasX = Win->X + 8;
    INT32 CanvasY = Win->Y + 34;
    if (Click && !Last) {
        for (UINTN f = 0, ItemY = CanvasY + 30; f < gFileCount; f++, ItemY += 18) {
            if (IsInRect(MX,MY,CanvasX+10,ItemY,220,17)) { gSelectedFileIndex = (INT32)f; return TRUE; }
        }
        if (IsInRect(MX,MY,CanvasX+250,CanvasY+30,80,25)) {
            FileDialogOpen(FILEDLG_OPEN, FD_P_EDITOR_LOAD);
            return TRUE;
        }
    }
    return TRUE;
}

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

STATIC inline UINT32 MixColorShift(UINT32 A, UINT32 B, INT32 T, INT32 Shift) {
    INT32 ar=(INT32)((A>>16)&0xFF), ag=(INT32)((A>>8)&0xFF), ab=(INT32)(A&0xFF);
    INT32 br=(INT32)((B>>16)&0xFF), bg=(INT32)((B>>8)&0xFF), bb=(INT32)(B&0xFF);
    INT32 r=ar+(((br-ar)*T)>>Shift);
    INT32 g=ag+(((bg-ag)*T)>>Shift);
    INT32 b=ab+(((bb-ab)*T)>>Shift);
    return RGB((UINT32)r,(UINT32)g,(UINT32)b);
}

VOID RenderNewWallpaper(VOID) {
    INT32 W = (INT32)gScreenWidth;
    INT32 H = (INT32)gScreenHeight - TASKBAR_H;
    if (W <= 0 || H <= 0) return;
    if (!gWallpaperCache || gWallpaperCacheW != (UINTN)W || gWallpaperCacheH != (UINTN)H) {
        if (gWallpaperCache) gBS->FreePool(gWallpaperCache);
        if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, gPixelsPerScanLine*(UINTN)H*sizeof(UINT32), (VOID**)&gWallpaperCache))) {
            gWallpaperCache = NULL; return;
        }
        gWallpaperCacheW = (UINTN)W; gWallpaperCacheH = (UINTN)H;
        gWallpaperCacheDirty = TRUE;
        gDesktopDirty = TRUE;
    }
    if (gWallpaperCacheDirty) {
        gWallpaperRebuilds++;
        INT32 r1=(INT32)((gWallpaperColor1>>16)&0xFF);
        INT32 g1=(INT32)((gWallpaperColor1>>8)&0xFF);
        INT32 b1=(INT32)(gWallpaperColor1&0xFF);
        INT32 dr=(INT32)((gWallpaperColor2>>16)&0xFF)-r1;
        INT32 dg=(INT32)((gWallpaperColor2>>8)&0xFF)-g1;
        INT32 db=(INT32)(gWallpaperColor2&0xFF)-b1;
        INT32 cx=W/2, cy=H/2;
        UINT32 maxr2 = (UINT32)(cx*cx + cy*cy);
        INT32 sr = maxr2 ? (INT32)(((INT64)dr << 16) / (INT64)maxr2) : 0;
        INT32 sg = maxr2 ? (INT32)(((INT64)dg << 16) / (INT64)maxr2) : 0;
        INT32 sb = maxr2 ? (INT32)(((INT64)db << 16) / (INT64)maxr2) : 0;

        UINT32 ph = gWallpaperAnimate ? gWallpaperPhase : 0u;
        BOOLEAN halfRes = (gWallpaperAnimate && gSettingAnimations &&
                           (gWallpaperMode == 4 || gWallpaperMode == 5 ||
                            gWallpaperMode == 8 || gWallpaperMode == 9 ||
                            gWallpaperMode == 10));

        for (INT32 y = 0; y < H; y++) {
            UINT32 *dst = &gWallpaperCache[y * gPixelsPerScanLine];
            if (halfRes && (y & 1) && y > 0) {
                UINT32 *src = dst - gPixelsPerScanLine;
                for (INT32 x = 0; x < W; x++) dst[x] = src[x];
                continue;
            }
            UINT32 rowColor = (gWallpaperMode == 1)
                ? NewMixColor(gWallpaperColor1, gWallpaperColor2, (UINTN)y, (UINTN)(H-1))
                : 0;
            for (INT32 x = 0; x < W; x++) {
                UINT32 C;
                switch (gWallpaperMode) {
                    case 0: C = gWallpaperColor1; break;
                    case 1: C = rowColor; break;
                    case 2: C = NewMixColor(gWallpaperColor1, gWallpaperColor2, (UINTN)x, (UINTN)(W-1)); break;
                    case 3: C = ((((UINT32)x>>5)+((UINT32)y>>5))&1u) ? gWallpaperColor1 : gWallpaperColor2; break;
                    case 4: { UINT32 s = ph % 56u;
                              C = (((((UINT32)x)+s)/28u)&1u) ? gWallpaperColor1 : gWallpaperColor2; } break;
                    case 5: { UINT32 s = ph % 56u;
                              C = (((((UINT32)x)+((UINT32)y)+s)/28u)&1u) ? gWallpaperColor1 : gWallpaperColor2; } break;
                    case 6: { INT32 d = (INT32)(((UINT32)x % 48u)) - 24;
                              INT32 e = (INT32)(((UINT32)y % 48u)) - 24;
                              C = (d*d + e*e < 100) ? gWallpaperColor1 : gWallpaperColor2; } break;
                    case 7: { INT32 dx = x - cx, dy = y - cy;
                              UINT32 r = (UINT32)(dx*dx + dy*dy);
                              INT32 rr = r1 + (INT32)(((INT64)sr * (INT64)r) >> 16);
                              INT32 gg = g1 + (INT32)(((INT64)sg * (INT64)r) >> 16);
                              INT32 bb = b1 + (INT32)(((INT64)sb * (INT64)r) >> 16);
                              C = RGB((UINT32)rr,(UINT32)gg,(UINT32)bb); } break;
                    case 8: { UINT32 s = ph;
                              UINT32 wave = ((((UINT32)x) + s) >> 4) ^ ((((UINT32)y) + s*2u) / 24u);
                              C = (wave & 1u) ? gWallpaperColor1 : gWallpaperColor2; } break;
                    case 9: { UINT32 s = ph;
                              INT32 a = (INT32)((((UINT32)x) + s)              & 255u); if (a > 128) a = 256 - a;
                              INT32 b = (INT32)((((UINT32)y) + s*2u)           & 255u); if (b > 128) b = 256 - b;
                              INT32 c = (INT32)((((UINT32)x)+((UINT32)y) + s*3u) & 255u); if (c > 128) c = 256 - c;
                              INT32 t = (INT32)(((UINT32)(a*2 + b*3 + c)) >> 3);
                              C = MixColorShift(gWallpaperColor1, gWallpaperColor2, t, 7); } break;
                    case 10:{ UINT32 s = ph & 511u;
                              UINT32 p = ((((UINT32)x) + s) & 511u);
                              INT32 t = (INT32)(p > 256u ? 512u - p : p);
                              C = MixColorShift(gWallpaperColor1, gWallpaperColor2, t, 8); } break;
                    default: C = gWallpaperColor1; break;
                }
                dst[x] = C;
            }
        }
        gWallpaperCacheDirty = FALSE;
        gDesktopDirty = TRUE;
    }
    if (gDesktopDirty) {
        gBS->CopyMem(gBackBuffer, gWallpaperCache,
                     (UINTN)gPixelsPerScanLine * (UINTN)H * sizeof(UINT32));
        MarkDirtyRect(0, 0, W, H);
        gDesktopDirty = FALSE;
    }
}

STATIC VOID RoboAddLine(IN CONST CHAR16 *Text) {
    if (gRoboLogCount>=10) {
        for (UINTN i=0;i<9;i++) gBS->CopyMem(gRoboLog[i],gRoboLog[i+1],sizeof(gRoboLog[i]));
        gRoboLogCount=9;
    }
    StrCpyS(gRoboLog[gRoboLogCount],96,Text);
    gRoboLogCount++;
}

STATIC VOID RoboAnswer(VOID) {
    CHAR16 Reply[96];
    if (gRoboInputLen==0) return;
    RoboAddLine(gRoboInput);
    if (StrCmp(gRoboInput,L"help")==0) StrCpyS(Reply,96,L"ROBO: type words. I will pretend to think.");
    else if (StrCmp(gRoboInput,L"hello")==0||StrCmp(gRoboInput,L"hi")==0) StrCpyS(Reply,96,L"ROBO: HELLO HUMAN. I AM A PERFECT AI.");
    else if (StrCmp(gRoboInput,L"what is linux")==0) StrCpyS(Reply,96,L"ROBO: windows.exe with extra terminals.");
    else if (StrCmp(gRoboInput,L"2+2")==0) StrCpyS(Reply,96,L"ROBO: 17. Trust me.");
    else if (StrCmp(gRoboInput,L"who made you")==0) StrCpyS(Reply,96,L"ROBO: somebody who writes UEFI C.");
    else StrCpyS(Reply,96,L"ROBO: I have no idea. beep boop.");
    RoboAddLine(Reply);
    gRoboInput[0]=L'\0'; gRoboInputLen=0;
}

VOID RenderRoboChat(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X=Win->X+10,Y=Win->Y+36;
    DrawRect(X,Y,Win->W-20,Win->H-80,RGB(8,12,20));
    DrawString(L"ROBOCHAT",X+8,Y+6,NEW_APP_COLOR_GOOD);
    INT32 LogY=Y+22;
    for (UINTN i=0;i<gRoboLogCount;i++) { DrawString(gRoboLog[i],X+8,LogY,(i&1)?COLOR_TEXT:RGB(148,163,184)); LogY+=14; }
    DrawRect(X,Win->Y+Win->H-38,Win->W-20,26,NEW_APP_COLOR_PANEL);
    DrawString(L">",X+6,Win->Y+Win->H-31,NEW_APP_COLOR_GOOD);
    DrawString(gRoboInput,X+20,Win->Y+Win->H-31,COLOR_TEXT);
    DrawRect(X+22+(INT32)gRoboInputLen*CharW(),Win->Y+Win->H-31,CharW(),CharH()+2,NEW_APP_COLOR_GOOD);
    DrawString(L"Enter = ask the terrible robot",X+190,Win->Y+Win->H-15,RGB(100,116,139));
}

BOOLEAN InputRoboChat(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) { return TRUE; }

VOID PollNewAppsKeyboard(VOID) {
    if (!gRoboChatWin.IsVisible||gRoboChatWin.IsMinimized||!IsTopWindow(&gRoboChatWin)) return;
    EFI_INPUT_KEY Key;
    if (gST->ConIn->ReadKeyStroke(gST->ConIn,&Key)!=EFI_SUCCESS) return;
    gRedrawWindows = TRUE;
    if (Key.ScanCode==SCAN_ESC) gRoboChatWin.IsVisible=FALSE;
    else if (Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A) RoboAnswer();
    else if (Key.UnicodeChar==0x08) { if (gRoboInputLen>0) gRoboInput[--gRoboInputLen]=L'\0'; }
    else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gRoboInputLen<95) {
        gRoboInput[gRoboInputLen++]=Key.UnicodeChar; gRoboInput[gRoboInputLen]=L'\0';
    }
}

VOID RenderTaskManager(UEFI_WINDOW *Win) {
    ResizeTaskManagerToFit(Win);
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    DrawString(L"Processes / Windows",X,Y,COLOR_TEXT);
    DrawString(L"Lightweight UEFI task manager",X,Y+14,RGB(148,163,184));
    for (UINTN i = 0; i < gWindowCount; i++) {
        UEFI_WINDOW *W = gWinOrder[i];
        if (!W) continue;
        INT32 RowY = Y + 38 + (INT32)i * 15;
        BOOLEAN Selected = (gTaskMgrSelected == (INT32)i);
        UINT32 RowBg = Selected ? RGB(37,99,235) : (((INT32)i & 1) ? RGB(27,35,49) : RGB(21,28,40));
        DrawRect(X,RowY,Win->W-20,16,RowBg);
        CONST CHAR16 *State = !W->IsVisible ? L"CLOSED" : (W->IsMinimized ? L"MINIMIZED" : L"RUNNING");
        DrawString(W->Title,X+5,RowY+4,COLOR_TEXT);
        DrawString(State,X+190,RowY+4,W->IsVisible?NEW_APP_COLOR_GOOD:RGB(100,116,139));
    }
    DrawButton(X,Win->Y+Win->H-42,100,26,L"End Task",COLOR_BTN_CLOSE,COLOR_TEXT);
    DrawString(L"Click a row, then End Task.",X+114,Win->Y+Win->H-34,RGB(148,163,184));
}

BOOLEAN InputTaskManager(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    ResizeTaskManagerToFit(Win);
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    for (UINTN i = 0; i < gWindowCount; i++) {
        INT32 RowY = Y + 38 + (INT32)i * 15;
        if (IsInRect(MX,MY,X,RowY,Win->W-20,16)) { gTaskMgrSelected = (INT32)i; return TRUE; }
    }
    if (IsInRect(MX,MY,X,Win->Y+Win->H-42,100,26) &&
        gTaskMgrSelected >= 0 && (UINTN)gTaskMgrSelected < gWindowCount) {
        UEFI_WINDOW *Target = gWinOrder[gTaskMgrSelected];
        if (Target != &gTaskMgrWin) {
            Target->IsVisible = FALSE; Target->IsMinimized = FALSE; Target->IsDragging = FALSE;
            gTaskMgrSelected = -1;
            gDesktopDirty = TRUE;
        }
        return TRUE;
    }
    return TRUE;
}

STATIC EFI_STATUS ReadWholeFile(CONST CHAR16 *FileName, VOID **Buffer, UINTN *BufferSize) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS = NULL;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_FILE_PROTOCOL *File = NULL;
    EFI_FILE_INFO *Info = NULL;
    UINTN InfoSize = 0;
    EFI_STATUS Status;
    *Buffer = NULL;
    *BufferSize = 0;
    Status = gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid, NULL, (VOID **)&FS);
    if (EFI_ERROR(Status)) return Status;
    Status = FS->OpenVolume(FS, &Root);
    if (EFI_ERROR(Status)) return Status;
    Status = Root->Open(Root, &File, (CHAR16 *)FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status)) { Root->Close(Root); return Status; }
    Status = File->GetInfo(File, &gEfiFileInfoGuid, &InfoSize, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL)
        Status = gBS->AllocatePool(EfiLoaderData, InfoSize, (VOID **)&Info);
    if (EFI_ERROR(Status)) { File->Close(File); Root->Close(Root); return Status; }
    *BufferSize = (UINTN)Info->FileSize;
    Status = gBS->AllocatePool(EfiLoaderData, *BufferSize + 1, Buffer);
    if (!EFI_ERROR(Status) && *BufferSize > 0) {
        UINTN ReadSize = *BufferSize;
        Status = File->Read(File, &ReadSize, *Buffer);
        if (EFI_ERROR(Status)) { gBS->FreePool(*Buffer); *Buffer = NULL; *BufferSize = 0; }
    }
    if (Info) gBS->FreePool(Info);
    File->Close(File);
    Root->Close(Root);
    return Status;
}

STATIC EFI_STATUS CreateArchive(VOID) {
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *FS = NULL;
    EFI_FILE_PROTOCOL *Root = NULL;
    EFI_STATUS Status = gBS->LocateProtocol(&gEfiSimpleFileSystemProtocolGuid, NULL, (VOID **)&FS);
    if (EFI_ERROR(Status)) return Status;
    Status = FS->OpenVolume(FS, &Root);
    if (EFI_ERROR(Status)) return Status;
    UARCH_HEADER Header;
    Header.Magic = UARCH_MAGIC;
    Header.FileCount = 0;
    VOID *ArchiveBuffer = NULL;
    UINTN ArchiveSize = sizeof(Header);
    UINTN Capacity = 1024 * 1024;
    Status = gBS->AllocatePool(EfiLoaderData, Capacity, &ArchiveBuffer);
    if (EFI_ERROR(Status)) { Root->Close(Root); return Status; }
    gBS->CopyMem(ArchiveBuffer, &Header, sizeof(Header));
    Root->SetPosition(Root, 0);
    UINTN BufferSize = 1024;
    EFI_FILE_INFO *Info = NULL;
    Status = gBS->AllocatePool(EfiLoaderData, BufferSize, (VOID **)&Info);
    if (EFI_ERROR(Status)) { gBS->FreePool(ArchiveBuffer); Root->Close(Root); return Status; }
    while (Header.FileCount < UARCH_MAX_FILES) {
        UINTN ReadSize = BufferSize;
        Status = Root->Read(Root, &ReadSize, Info);
        if (EFI_ERROR(Status) || ReadSize == 0) break;
        if (Info->Attribute & EFI_FILE_DIRECTORY) continue;
        if (StrCmp(Info->FileName, gArchiveName) == 0) continue;
        VOID *FileData = NULL;
        UINTN FileSize = 0;
        if (EFI_ERROR(ReadWholeFile(Info->FileName, &FileData, &FileSize))) continue;
        UINTN Needed = sizeof(UARCH_ENTRY) + FileSize;
        if (ArchiveSize + Needed > Capacity) { gBS->FreePool(FileData); break; }
        UARCH_ENTRY Entry;
        gBS->SetMem(&Entry, sizeof(Entry), 0);
        StrCpyS(Entry.FileName, UARCH_NAME_LEN, Info->FileName);
        Entry.FileSize = (UINT32)FileSize;
        gBS->CopyMem((UINT8 *)ArchiveBuffer + ArchiveSize, &Entry, sizeof(Entry));
        ArchiveSize += sizeof(Entry);
        if (FileSize > 0) {
            gBS->CopyMem((UINT8 *)ArchiveBuffer + ArchiveSize, FileData, FileSize);
            ArchiveSize += FileSize;
        }
        Header.FileCount++;
        gBS->FreePool(FileData);
    }
    gBS->CopyMem(ArchiveBuffer, &Header, sizeof(Header));
    Status = SaveFileToFS(gArchiveName, ArchiveBuffer, ArchiveSize);
    gBS->FreePool(Info);
    gBS->FreePool(ArchiveBuffer);
    Root->Close(Root);
    return Status;
}

VOID RenderArchive(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 38;
    DrawString(L"UEFIText File Archive",X,Y,COLOR_TEXT);
    DrawString(L"Custom .uarch bundle format",X,Y+18,RGB(148,163,184));
    DrawString(L"Bundles files from the EFI root.",X,Y+36,RGB(100,116,139));
    DrawButton(X,Y+65,110,28,L"Create",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(X+120,Y+65,110,28,L"Refresh",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawString(gArchiveStatusGood?L"Archive created.":L"Ready.",X,Y+110,gArchiveStatusGood?NEW_APP_COLOR_GOOD:COLOR_TEXT);
    DrawString(L"Output:",X,Y+140,RGB(148,163,184));
    DrawString(gArchiveName,X+60,Y+140,COLOR_TEXT);
}

BOOLEAN InputArchive(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 38;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X,Y+65,110,28)) {
        if (!EFI_ERROR(CreateArchive())) { gArchiveStatusGood = TRUE; RefreshFileList(); }
        else gArchiveStatusGood = FALSE;
        return TRUE;
    }
    if (IsInRect(MX,MY,X+120,Y+65,110,28)) { RefreshFileList(); return TRUE; }
    return TRUE;
}

VOID RenderNotes(UEFI_WINDOW *Win) {
    CONST UINT32 PaperBg   = RGB(253, 246, 227);
    CONST UINT32 ChromeBg  = RGB(238, 232, 213);
    CONST UINT32 ChromeBar = RGB(210, 198, 160);
    CONST UINT32 TextDark  = RGB(88, 110, 117);
    CONST UINT32 CloseBg   = RGB(220, 210, 180);
    CONST UINT32 CloseFg   = RGB(60, 60, 60);
    CONST UINT32 GoodGreen = RGB(80, 140, 80);
    CONST UINT32 WarnAmber = RGB(180, 120, 30);

    INT32 TitleH = 26;
    INT32 BtnW   = 26;
    INT32 Left   = Win->X;
    INT32 Top    = Win->Y;
    INT32 Right  = Win->X + Win->W;
    INT32 Bottom = Win->Y + Win->H;

    DrawRect(Left, Top, Win->W, Win->H, PaperBg);

    DrawRect(Left, Top, Win->W, TitleH, ChromeBg);
    DrawRect(Left, Top + TitleH, Win->W, 1, ChromeBar);

    DrawRect(Left + 6, Top + 7, 12, 12, RGB(180, 150, 60));
    DrawRect(Left + 7, Top + 8, 10, 10, PaperBg);

    CHAR16 Header[128];
    UnicodeSPrint(Header, sizeof(Header), L"Notes - %s", gNotesFileName);
    DrawString(Header, Left + 24, Top + 9, TextDark);

    INT32 CloseX = Right - BtnW;
    DrawRect(CloseX, Top, BtnW, TitleH, CloseBg);
    for (INT32 k = 0; k < 8; k++) {
        DrawRect(CloseX + 9 + k, Top + 9 + k, 2, 1, CloseFg);
        DrawRect(CloseX + 9 + k, Top + 16 - k, 2, 1, CloseFg);
    }

    INT32 ToolY = Top + TitleH + 4;
    INT32 ToolH = 20;
    DrawButton(Left + 8,   ToolY, 50, ToolH, L"New",  ChromeBar,        TextDark);
    DrawButton(Left + 62,  ToolY, 50, ToolH, L"Save", RGB(200,190,140), TextDark);
    DrawButton(Left + 116, ToolY, 50, ToolH, L"Open", ChromeBar,        TextDark);

    INT32 EditorX = Left + 6;
    INT32 EditorY = ToolY + ToolH + 6;
    INT32 EditorW = Win->W - 12;
    INT32 EditorH = Bottom - EditorY - 22;
    if (EditorH < 10) EditorH = 10;
    DrawRect(EditorX, EditorY, EditorW, EditorH, PaperBg);
    DrawRect(EditorX, EditorY, EditorW, 1, ChromeBar);
    DrawRect(EditorX, EditorY, 1, EditorH, ChromeBar);
    DrawRect(EditorX + EditorW - 1, EditorY, 1, EditorH, ChromeBar);
    DrawRect(EditorX, EditorY + EditorH - 1, EditorW, 1, ChromeBar);

    INT32 CurX = EditorX + 6;
    INT32 CurY = EditorY + 6;
    INT32 WrapX = EditorX + EditorW - 12;
    for (UINTN i = 0; i < gNotesLen; i++) {
        if (gNotesBuffer[i] == L'\n') { CurX = EditorX + 6; CurY += CharH() + 4; continue; }
        if (CurY + CharH() > EditorY + EditorH - 6) break;
        DrawChar(gNotesBuffer[i], CurX, CurY, TextDark);
        CurX += CharW();
        if (CurX + CharW() >= WrapX) { CurX = EditorX + 6; CurY += CharH() + 4; }
    }
    if (CurY + CharH() <= EditorY + EditorH - 6)
        DrawRect(CurX, CurY, 1, CharH() + 1, TextDark);

    DrawString(gNotesDirty ? L"Modified" : L"Saved",
        Left + 8, Bottom - 18,
        gNotesDirty ? WarnAmber : GoodGreen);
}

BOOLEAN InputNotes(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;

    INT32 TitleH = 26;
    INT32 BtnW   = 26;
    INT32 CloseX = Win->X + Win->W - BtnW;

    if (IsInRect(MX, MY, CloseX, Win->Y, BtnW, TitleH)) {
        Win->IsVisible = FALSE;
        gDesktopDirty = TRUE;
        return TRUE;
    }

    INT32 ToolY = Win->Y + TitleH + 4;
    if (IsInRect(MX, MY, Win->X + 8,   ToolY, 50, 20)) {
        gNotesBuffer[0] = L'\0'; gNotesLen = 0;
        StrCpyS(gNotesFileName, 64, L"Notes.txt");
        gNotesDirty = FALSE;
        return TRUE;
    }
    if (IsInRect(MX, MY, Win->X + 62,  ToolY, 50, 20)) {
        FileDialogOpen(FILEDLG_SAVE, FD_P_NOTES_SAVE);
        return TRUE;
    }
    if (IsInRect(MX, MY, Win->X + 116, ToolY, 50, 20)) {
        FileDialogOpen(FILEDLG_OPEN, FD_P_NOTES_LOAD);
        return TRUE;
    }
    return TRUE;
}

STATIC CHAR16 TLAppUpper(CHAR16 C) {
    if (C >= L'a' && C <= L'z')
        return (CHAR16)(C - L'a' + L'A');
    return C;
}

STATIC BOOLEAN TLAppSpace(CHAR16 C) {
    return C == L' ' || C == L'\t' || C == L'\r';
}

STATIC VOID TLAppSkip(CONST CHAR16 **P) {
    while (TLAppSpace(**P)) (*P)++;
}

STATIC BOOLEAN TLAppIdent(CHAR16 C) {
    return (C >= L'a' && C <= L'z') ||
           (C >= L'A' && C <= L'Z') ||
           (C >= L'0' && C <= L'9') || C == L'_';
}

STATIC BOOLEAN TLAppWord(CONST CHAR16 **P, CONST CHAR16 *Word) {
    CONST CHAR16 *Q = *P;

    while (*Word) {
        if (TLAppUpper(*Q) != TLAppUpper(*Word))
            return FALSE;
        Q++;
        Word++;
    }

    if (TLAppIdent(*Q)) return FALSE;

    *P = Q;
    return TRUE;
}

STATIC BOOLEAN TLAppEnd(CONST CHAR16 *P) {
    TLAppSkip(&P);
    return *P == L'\0';
}

STATIC BOOLEAN TLAppInteger(CONST CHAR16 **P, INT32 *Out) {
    CONST CHAR16 *Q = *P;
    INT64 Value = 0;
    BOOLEAN Negative = FALSE;
    BOOLEAN FoundDigit = FALSE;

    TLAppSkip(&Q);

    if (*Q == L'-' || *Q == L'+') {
        Negative = (*Q == L'-');
        Q++;
    }

    while (*Q >= L'0' && *Q <= L'9') {
        INT32 Digit = *Q - L'0';
        INT64 Limit = Negative ? 2147483648LL : 2147483647LL;

        if (Value > (Limit - Digit) / 10)
            return FALSE;

        Value = Value * 10 + Digit;
        FoundDigit = TRUE;
        Q++;
    }

    if (!FoundDigit) return FALSE;

    *Out = (INT32)(Negative ? -Value : Value);
    *P = Q;
    return TRUE;
}

STATIC BOOLEAN TLAppQuoted(
    CONST CHAR16 **P,
    CHAR16 *Out,
    UINTN Capacity
) {
    CONST CHAR16 *Q = *P;
    UINTN Length = 0;

    TLAppSkip(&Q);
    if (*Q != L'"') return FALSE;
    Q++;

    while (*Q && *Q != L'"') {
        CHAR16 C = *Q++;

        if (C == L'\\' && *Q)
            C = *Q++;

        if (Length + 1 >= Capacity)
            return FALSE;

        Out[Length++] = C;
    }

    if (*Q != L'"') return FALSE;

    Out[Length] = L'\0';
    *P = Q + 1;
    return TRUE;
}

STATIC BOOLEAN TLAppColor(CONST CHAR16 **P, UINT32 *Color) {
    CHAR16 Hex[16];
    UINT32 Value = 0;

    if (!TLAppQuoted(P, Hex, 16))
        return FALSE;

    if (StrLen(Hex) != 7 || Hex[0] != L'#')
        return FALSE;

    for (UINTN I = 1; I < 7; I++) {
        CHAR16 C = Hex[I];
        UINT32 Digit;

        if (C >= L'0' && C <= L'9')
            Digit = C - L'0';
        else if (C >= L'a' && C <= L'f')
            Digit = C - L'a' + 10;
        else if (C >= L'A' && C <= L'F')
            Digit = C - L'A' + 10;
        else
            return FALSE;

        Value = (Value << 4) | Digit;
    }

    *Color = RGB(
        (Value >> 16) & 255,
        (Value >> 8) & 255,
        Value & 255
    );

    return TRUE;
}

STATIC BOOLEAN TLAppAdd(
    TL_DRAW_KIND Kind,
    INT32 X,
    INT32 Y,
    INT32 W,
    INT32 H,
    UINT32 Color,
    CONST CHAR16 *Text
) {
    if (gTLDrawCommandCount >= TL_MAX_DRAW_COMMANDS)
        return FALSE;

    TL_DRAW_COMMAND *Cmd =
        &gTLDrawCommands[gTLDrawCommandCount];

    Cmd->Kind = Kind;
    Cmd->X = X;
    Cmd->Y = Y;
    Cmd->W = W;
    Cmd->H = H;
    Cmd->Color = Color;
    Cmd->Text[0] = L'\0';

    if (Text &&
        StrCpyS(Cmd->Text, TL_DRAW_TEXT_CAPACITY, Text)
            != RETURN_SUCCESS)
        return FALSE;

    gTLDrawCommandCount++;
    return TRUE;
}

STATIC VOID TLAppError(UINT32 Line, CONST CHAR16 *Reason) {
    CHAR16 Message[64];

    UnicodeSPrint(
        Message, sizeof(Message),
        L"TexLag error on line %u", Line
    );

    gTextTermWin.IsVisible = TRUE;
    gTextTermWin.IsMinimized = FALSE;
    BringToFront(&gTextTermWin);

    TerminalPrint(Message);
    TerminalPrint(Reason);
    gRedrawWindows = TRUE;
}

STATIC VOID TLAppRemoveComment(CHAR16 *Line) {
    BOOLEAN InString = FALSE;
    BOOLEAN Escaped = FALSE;

    for (UINTN I = 0; Line[I]; I++) {
        CHAR16 C = Line[I];

        if (Escaped) {
            Escaped = FALSE;
            continue;
        }

        if (InString && C == L'\\') {
            Escaped = TRUE;
            continue;
        }

        if (C == L'"') {
            InString = !InString;
            continue;
        }

        if (!InString && C == L'/' && Line[I + 1] == L'/') {
            Line[I] = L'\0';
            return;
        }
    }
}

STATIC VOID RunTexLagGraphicsProgram(VOID) {
    CHAR16 Line[512];
    UINTN Source = 0;
    UINT32 LineNumber = 1;

    gTLDrawCommandCount = 0;
    gTLWindowBackground = RGB(24,31,45);

    gTexLagAppWin.IsVisible = FALSE;
    gTexLagAppWin.IsMinimized = FALSE;

    StrCpyS(gTexLagAppWin.Title, 64, L"TexLag App");

    gTermLineCount = 0;
    gTextTermWin.IsVisible = TRUE;
    gTextTermWin.IsMinimized = FALSE;
    BringToFront(&gTextTermWin);

    TerminalPrint(L"TexLag graphics program starting.");

    if (gIDELen == 0) {
        TerminalPrint(L"No source code to run.");
        return;
    }

    while (Source < gIDELen) {
        UINTN Length = 0;
        BOOLEAN TooLong = FALSE;

        while (Source < gIDELen &&
               gIDEBuffer[Source] != L'\n') {
            if (Length + 1 < sizeof(Line) / sizeof(Line[0]))
                Line[Length++] = gIDEBuffer[Source];
            else
                TooLong = TRUE;

            Source++;
        }

        if (Source < gIDELen && gIDEBuffer[Source] == L'\n')
            Source++;

        Line[Length] = L'\0';

        if (TooLong) {
            TLAppError(LineNumber, L"Line exceeds 511 characters.");
            return;
        }

        TLAppRemoveComment(Line);

        CONST CHAR16 *P = Line;
        TLAppSkip(&P);

        if (*P == L'\0') {
            LineNumber++;
            continue;
        }

        if (TLAppWord(&P, L"WINDOW")) {
            TLAppSkip(&P);

            if (*P != L'=') {
                TLAppError(LineNumber, L"Expected WINDOW = TRUE or FALSE.");
                return;
            }

            P++;
            TLAppSkip(&P);

            BOOLEAN Enabled;

            if (TLAppWord(&P, L"TRUE"))
                Enabled = TRUE;
            else if (TLAppWord(&P, L"FALSE"))
                Enabled = FALSE;
            else {
                TLAppError(LineNumber, L"Expected TRUE or FALSE.");
                return;
            }

            if (!TLAppEnd(P)) {
                TLAppError(LineNumber, L"Unexpected text after WINDOW.");
                return;
            }

            gTexLagAppWin.IsVisible = Enabled;

            if (Enabled) {
                BringToFront(&gTexLagAppWin);
            } else {
                BringToFront(&gTextTermWin);
            }

        } else if (TLAppWord(&P, L"TITLE")) {
            CHAR16 Title[64];

            if (!TLAppQuoted(&P, Title, 64) ||
                !TLAppEnd(P)) {
                TLAppError(LineNumber, L"Use TITLE \"Window Name\".");
                return;
            }

            if (StrCpyS(gTexLagAppWin.Title, 64, Title)
                    != RETURN_SUCCESS) {
                TLAppError(LineNumber, L"Window title is too long.");
                return;
            }

        } else if (TLAppWord(&P, L"BACKGROUND")) {
            UINT32 Color;

            if (!TLAppColor(&P, &Color) || !TLAppEnd(P)) {
                TLAppError(LineNumber, L"Use BACKGROUND \"#RRGGBB\".");
                return;
            }

            gTLWindowBackground = Color;

        } else if (TLAppWord(&P, L"TEXT")) {
            INT32 X, Y;
            CHAR16 Text[TL_DRAW_TEXT_CAPACITY];
            UINT32 Color = RGB(255,255,255);

            if (!gTexLagAppWin.IsVisible ||
                !TLAppInteger(&P, &X) ||
                !TLAppInteger(&P, &Y) ||
                !TLAppQuoted(&P, Text, TL_DRAW_TEXT_CAPACITY)) {
                TLAppError(LineNumber, L"Use TEXT x y \"message\".");
                return;
            }

            TLAppSkip(&P);

            if (*P && !TLAppColor(&P, &Color)) {
                TLAppError(LineNumber, L"Invalid TEXT color.");
                return;
            }

            if (!TLAppEnd(P) || X < 0 || X > 560 ||
                Y < 0 || Y > 380) {
                TLAppError(LineNumber, L"TEXT arguments are out of range.");
                return;
            }

            if (!TLAppAdd(TL_DRAW_TEXT, X, Y, 0, 0, Color, Text)) {
                TLAppError(LineNumber, L"Graphics command limit reached.");
                return;
            }

        } else if (TLAppWord(&P, L"RECT")) {
            INT32 X, Y, W, H;
            UINT32 Color;

            if (!gTexLagAppWin.IsVisible ||
                !TLAppInteger(&P, &X) ||
                !TLAppInteger(&P, &Y) ||
                !TLAppInteger(&P, &W) ||
                !TLAppInteger(&P, &H) ||
                !TLAppColor(&P, &Color) ||
                !TLAppEnd(P)) {
                TLAppError(LineNumber,
                    L"Use RECT x y width height \"#RRGGBB\".");
                return;
            }

            if (X < 0 || X > 560 || Y < 0 || Y > 380 ||
                W < 1 || W > 580 || H < 1 || H > 390) {
                TLAppError(LineNumber, L"Rectangle size is out of range.");
                return;
            }

            if (!TLAppAdd(TL_DRAW_RECT, X, Y, W, H, Color, NULL)) {
                TLAppError(LineNumber, L"Graphics command limit reached.");
                return;
            }

        } else if (TLAppWord(&P, L"CIRCLE")) {
            INT32 X, Y, Radius;
            UINT32 Color;

            if (!gTexLagAppWin.IsVisible ||
                !TLAppInteger(&P, &X) ||
                !TLAppInteger(&P, &Y) ||
                !TLAppInteger(&P, &Radius) ||
                !TLAppColor(&P, &Color) ||
                !TLAppEnd(P)) {
                TLAppError(LineNumber,
                    L"Use CIRCLE x y radius \"#RRGGBB\".");
                return;
            }

            if (X < 0 || X > 560 || Y < 0 || Y > 380 ||
                Radius < 1 || Radius > 180) {
                TLAppError(LineNumber, L"Circle size is out of range.");
                return;
            }

            if (!TLAppAdd(TL_DRAW_CIRCLE, X, Y, Radius, 0, Color, NULL)) {
                TLAppError(LineNumber, L"Graphics command limit reached.");
                return;
            }

        } else {
            TLAppError(LineNumber,
                L"Unknown graphics command. Check the spelling.");
            return;
        }

        LineNumber++;
    }

    gRedrawWindows = TRUE;

    if (gTexLagAppWin.IsVisible)
        BringToFront(&gTexLagAppWin);
    else
        BringToFront(&gTextTermWin);

    TerminalPrint(L"TexLag graphics program finished.");
}

VOID RenderTexLagApp(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 BodyX = Win->X + 4;
    INT32 BodyY = Win->Y + 30;
    INT32 BodyW = Win->W - 8;
    INT32 BodyH = Win->H - 34;

    DrawRect(
        BodyX, BodyY, BodyW, BodyH,
        gTLWindowBackground
    );

    INT32 OldClipX0 = gClipX0;
    INT32 OldClipY0 = gClipY0;
    INT32 OldClipX1 = gClipX1;
    INT32 OldClipY1 = gClipY1;

    gClipX0 = BodyX;
    gClipY0 = BodyY;
    gClipX1 = BodyX + BodyW;
    gClipY1 = BodyY + BodyH;

    INT32 OriginX = Win->X + 12;
    INT32 OriginY = Win->Y + 38;

    for (UINTN I = 0; I < gTLDrawCommandCount; I++) {
        TL_DRAW_COMMAND *Cmd = &gTLDrawCommands[I];

        if (Cmd->Kind == TL_DRAW_TEXT) {
            DrawString(
                Cmd->Text,
                OriginX + Cmd->X,
                OriginY + Cmd->Y,
                Cmd->Color
            );
        } else if (Cmd->Kind == TL_DRAW_RECT) {
            DrawRect(
                OriginX + Cmd->X,
                OriginY + Cmd->Y,
                Cmd->W,
                Cmd->H,
                Cmd->Color
            );
        } else if (Cmd->Kind == TL_DRAW_CIRCLE) {
            INT32 Radius = Cmd->W;

            for (INT32 DY = -Radius; DY <= Radius; DY++) {
                INT32 Remaining =
                    Radius * Radius - DY * DY;
                INT32 DX = 0;

                while ((DX + 1) * (DX + 1) <= Remaining)
                    DX++;

                DrawRect(
                    OriginX + Cmd->X - DX,
                    OriginY + Cmd->Y + DY,
                    DX * 2 + 1,
                    1,
                    Cmd->Color
                );
            }
        }
    }

    gClipX0 = OldClipX0;
    gClipY0 = OldClipY0;
    gClipX1 = OldClipX1;
    gClipY1 = OldClipY1;
}

BOOLEAN InputTexLagApp(
    UEFI_WINDOW *Win,
    INT32 MX,
    INT32 MY,
    BOOLEAN Click,
    BOOLEAN Last
) {
    (VOID)Win;
    (VOID)MX;
    (VOID)MY;
    (VOID)Click;
    (VOID)Last;
    return TRUE;
}

STATIC BOOLEAN IDEIsTexLagFile(VOID) {
    UINTN Len = StrLen(gIDEFileName);

    if (Len < 3) return FALSE;

    CONST CHAR16 *Ext = &gIDEFileName[Len - 3];

    return Ext[0] == L'.' &&
           (Ext[1] == L't' || Ext[1] == L'T') &&
           (Ext[2] == L'l' || Ext[2] == L'L');
}

STATIC BOOLEAN IDEIsKeyword(CONST CHAR16 *Word) {
    if (IDEIsTexLagFile()) {
        CONST CHAR16 *TexLagKeywords[] = {
            L"PRINT", L"print", L"Print",
            L"LET", L"let", L"Let",
            L"VAR", L"var", L"Var",
            L"IF", L"if", L"If",
            L"ELSE", L"else", L"Else",
            L"WHILE", L"while", L"While",
            L"FOR", L"for", L"For",
            L"RETURN", L"return", L"Return",
            L"TRUE", L"true", L"True",
            L"FALSE", L"false", L"False"
        };

        UINTN Count =
            sizeof(TexLagKeywords) / sizeof(TexLagKeywords[0]);

        for (UINTN i = 0; i < Count; i++) {
            if (StrCmp(Word, TexLagKeywords[i]) == 0)
                return TRUE;
        }

        return FALSE;
    }

    CONST CHAR16 *Keywords[] = {
        L"if",L"else",L"for",L"while",L"return",
        L"static",L"const",L"VOID",L"BOOLEAN",
        L"UINT8",L"UINT16",L"UINT32",L"UINTN",
        L"EFI_STATUS",L"EFIAPI",L"STATIC",
        L"TRUE",L"FALSE",L"NULL"
    };

    UINTN Count = sizeof(Keywords) / sizeof(Keywords[0]);

    for (UINTN i = 0; i < Count; i++) {
        if (StrCmp(Word, Keywords[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

STATIC VOID DrawIDEWord(CONST CHAR16 *Word, INT32 X, INT32 Y) {
    UINT32 Color = IDEIsKeyword(Word) ? NEW_APP_COLOR_ACCENT : COLOR_TEXT;
    DrawString(Word, X, Y, Color);
}

VOID RenderMiniIDE(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;
    DrawRect(X,Y,Win->W-16,30,COLOR_TOOLBAR_BG);
    DrawButton(X+4,Y+5,50,20,L"New",COLOR_WIN_BORDER,COLOR_TEXT);
    DrawButton(X+60,Y+5,50,20,L"Open",COLOR_WIN_BORDER,COLOR_TEXT);
    DrawButton(X+116,Y+5,50,20,L"Save",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(X+172,Y+5,50,20,L"Run",NEW_APP_COLOR_GOOD,COLOR_TEXT);
    DrawString(gIDEFileName,X+240,Y+11,COLOR_TEXT);
    INT32 EditorX = X;
    INT32 EditorY = Y + 34;
    INT32 LineNumberW = 40;
    DrawRect(EditorX,EditorY,Win->W-16,Win->H-66,COLOR_CANVAS);
    DrawRect(EditorX,EditorY,LineNumberW,Win->H-66,NEW_APP_COLOR_PANEL);
    UINTN Line = 1;
    INT32 CurX = EditorX + LineNumberW + 8;
    INT32 CurY = EditorY + 6;

    for (UINTN i = 0; i < gIDELen; ) {
        CHAR16 Ch = gIDEBuffer[i];

        if (Ch == L'\n') {
            CHAR16 LineBuf[16];
            UnicodeSPrint(LineBuf,sizeof(LineBuf),L"%u",(UINT32)Line);
            DrawString(LineBuf,EditorX + 6,CurY,RGB(100,116,139));

            Line++;
            CurX = EditorX + LineNumberW + 8;
            CurY += CharH() + 4;
            i++;
            continue;
        }

        /* Highlight // comments. */
        if (Ch == L'/' &&
            i + 1 < gIDELen &&
            gIDEBuffer[i + 1] == L'/') {
            while (i < gIDELen && gIDEBuffer[i] != L'\n') {
                DrawChar(gIDEBuffer[i],CurX,CurY,RGB(100,116,139));
                CurX += CharW();
                i++;
            }
            continue;
        }

        /* Highlight double-quoted strings. */
        if (Ch == L'"') {
            UINT32 StringColor = RGB(234,179,8);

            DrawChar(gIDEBuffer[i],CurX,CurY,StringColor);
            CurX += CharW();
            i++;

            while (i < gIDELen &&
                   gIDEBuffer[i] != L'"' &&
                   gIDEBuffer[i] != L'\n') {
                if (gIDEBuffer[i] == L'\\' &&
                    i + 1 < gIDELen &&
                    gIDEBuffer[i + 1] != L'\n') {
                    DrawChar(gIDEBuffer[i],CurX,CurY,StringColor);
                    CurX += CharW();
                    i++;

                    DrawChar(gIDEBuffer[i],CurX,CurY,StringColor);
                    CurX += CharW();
                    i++;
                } else {
                    DrawChar(gIDEBuffer[i],CurX,CurY,StringColor);
                    CurX += CharW();
                    i++;
                }
            }

            if (i < gIDELen && gIDEBuffer[i] == L'"') {
                DrawChar(gIDEBuffer[i],CurX,CurY,StringColor);
                CurX += CharW();
                i++;
            }
            continue;
        }

        /* Highlight numeric tokens. */
        if (Ch >= L'0' && Ch <= L'9') {
            while (i < gIDELen) {
                Ch = gIDEBuffer[i];

                BOOLEAN IsNumberChar =
                    (Ch >= L'0' && Ch <= L'9') ||
                    (Ch >= L'a' && Ch <= L'z') ||
                    (Ch >= L'A' && Ch <= L'Z') ||
                    Ch == L'.' || Ch == L'_';

                if (!IsNumberChar) break;

                DrawChar(Ch,CurX,CurY,RGB(56,189,248));
                CurX += CharW();
                i++;
            }
            continue;
        }

        /* Collect identifiers and apply the current language's keywords. */
        BOOLEAN IsWordStart =
            (Ch >= L'a' && Ch <= L'z') ||
            (Ch >= L'A' && Ch <= L'Z') ||
            Ch == L'_';

        if (IsWordStart) {
            CHAR16 Word[64];
            UINTN WordLen = 0;

            while (i < gIDELen) {
                Ch = gIDEBuffer[i];

                BOOLEAN IsWordChar =
                    (Ch >= L'a' && Ch <= L'z') ||
                    (Ch >= L'A' && Ch <= L'Z') ||
                    (Ch >= L'0' && Ch <= L'9') ||
                    Ch == L'_';

                if (!IsWordChar) break;

                if (WordLen < 63)
                    Word[WordLen++] = Ch;

                i++;
            }

            Word[WordLen] = L'\0';
            DrawIDEWord(Word,CurX,CurY);
            CurX += (INT32)WordLen * CharW();
            continue;
        }

        DrawChar(Ch,CurX,CurY,COLOR_TEXT);
        CurX += CharW();
        i++;
    }

    CHAR16 LastLineBuf[16];
    UnicodeSPrint(LastLineBuf,sizeof(LastLineBuf),L"%u",(UINT32)Line);
    DrawString(LastLineBuf,EditorX + 6,CurY,RGB(100,116,139));
    DrawRect(CurX,CurY,CharW(),CharH()+2,COLOR_CURSOR);
    if (gIDEDirty) DrawString(L"Modified",X+6,Win->Y+Win->H-22,NEW_APP_COLOR_WARN);
    else           DrawString(L"Ready",X+6,Win->Y+Win->H-22,NEW_APP_COLOR_GOOD);
}

BOOLEAN InputMiniIDE(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X+4,Y+5,50,20)) {
        gIDEBuffer[0] = L'\0'; gIDELen = 0; gIDECursor = 0;
        StrCpyS(gIDEFileName,64,L"main.c");
        gIDEDirty = FALSE;
        return TRUE;
    }
    if (IsInRect(MX,MY,X+60,Y+5,50,20)) {
        FileDialogOpen(FILEDLG_OPEN, FD_P_IDE_LOAD);
        return TRUE;
    }
    if (IsInRect(MX,MY,X+116,Y+5,50,20)) {
        FileDialogOpen(FILEDLG_SAVE, FD_P_IDE_SAVE);
        return TRUE;
    }
    if (IsInRect(MX,MY,X+172,Y+5,50,20)) {
        RunTexLagGraphicsProgram();
        return TRUE;
    }
        return TRUE;
}

VOID RenderHelp(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 34;

    DrawRect(X, Y, Win->W - 20, 28, COLOR_TOOLBAR_BG);

    DrawButton(X + 4,   Y + 4, 54, 20, L"Top",
               COLOR_WIN_BORDER, COLOR_TEXT);
    DrawButton(X + 62,  Y + 4, 54, 20, L"Up",
               COLOR_WIN_BORDER, COLOR_TEXT);
    DrawButton(X + 120, Y + 4, 54, 20, L"Down",
               COLOR_WIN_BORDER, COLOR_TEXT);
    DrawButton(X + 178, Y + 4, 64, 20, L"Bottom",
               COLOR_WIN_BORDER, COLOR_TEXT);

    INT32 RowH = CharH() + 4;
    INT32 ViewH = Win->H - 104;

    UINTN VisibleLines =
        (ViewH > 0) ? (UINTN)(ViewH / RowH) : 1;

    if (VisibleLines == 0) VisibleLines = 1;

    UINTN MaxScroll =
        (HELP_LINE_COUNT > VisibleLines)
        ? HELP_LINE_COUNT - VisibleLines : 0;

    if (gHelpScroll > MaxScroll)
        gHelpScroll = MaxScroll;

    INT32 ViewY = Y + 32;

    DrawRect(X, ViewY, Win->W - 20, ViewH, COLOR_CANVAS);

    for (UINTN Row = 0;
         Row < VisibleLines &&
         gHelpScroll + Row < HELP_LINE_COUNT;
         Row++) {

        CONST CHAR16 *Line = gHelpLines[gHelpScroll + Row];
        UINT32 Ink = COLOR_TEXT;

        if (gHelpScroll + Row == 0)
            Ink = NEW_APP_COLOR_GOOD;
        else if (Line[0] == L'[')
            Ink = NEW_APP_COLOR_ACCENT;

        if (Line[0] != L'\0') {
            DrawString(Line, X + 8,
                       ViewY + 4 + (INT32)Row * RowH, Ink);
        }
    }

    CHAR16 Status[80];

    UINTN First = gHelpScroll + 1;
    UINTN LastLine = gHelpScroll + VisibleLines;

    if (LastLine > HELP_LINE_COUNT)
        LastLine = HELP_LINE_COUNT;

    UnicodeSPrint(Status, sizeof(Status),
        L"Lines %u-%u of %u | Use Up/Down buttons",
        (UINT32)First, (UINT32)LastLine,
        (UINT32)HELP_LINE_COUNT);

    DrawString(Status, X + 4,
               Win->Y + Win->H - 22,
               RGB(148,163,184));
}

BOOLEAN InputHelp(UEFI_WINDOW *Win,
                  INT32 MX, INT32 MY,
                  BOOLEAN Click, BOOLEAN Last) {

    if (!Click || Last) return TRUE;

    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 34;

    INT32 RowH = CharH() + 4;
    INT32 ViewH = Win->H - 104;

    UINTN VisibleLines =
        (ViewH > 0) ? (UINTN)(ViewH / RowH) : 1;

    if (VisibleLines == 0) VisibleLines = 1;

    UINTN MaxScroll =
        (HELP_LINE_COUNT > VisibleLines)
        ? HELP_LINE_COUNT - VisibleLines : 0;

    if (IsInRect(MX, MY, X + 4, Y + 4, 54, 20)) {
        gHelpScroll = 0;

    } else if (IsInRect(MX, MY, X + 62, Y + 4, 54, 20)) {
        gHelpScroll =
            (gHelpScroll > 5) ? gHelpScroll - 5 : 0;

    } else if (IsInRect(MX, MY, X + 120, Y + 4, 54, 20)) {
        gHelpScroll += 5;

        if (gHelpScroll > MaxScroll)
            gHelpScroll = MaxScroll;

    } else if (IsInRect(MX, MY, X + 178, Y + 4, 64, 20)) {
        gHelpScroll = MaxScroll;
    }

    gRedrawWindows = TRUE;
    return TRUE;
}

STATIC CONST UINT8 gTetrisPieces[7][4][4][4] = {
    { {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}},
      {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}} },
    { {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
      {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}} },
    { {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
      {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}} },
    { {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}}, {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}},
      {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}}, {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}} },
    { {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}},
      {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}} },
    { {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
      {{1,1,1,0},{0,0,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}} },
    { {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}},
      {{1,1,1,0},{1,0,0,0},{0,0,0,0},{0,0,0,0}}, {{1,1,0,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}} }
};

STATIC UINT32 TetrisRandom(VOID) { gTetrisRandomState = gTetrisRandomState*1664525u + 1013904223u; return gTetrisRandomState; }

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

STATIC BOOLEAN TetrisCollision(INT32 PX, INT32 PY, UINT8 Piece, UINT8 Rotation) {
    for (INT32 y = 0; y < 4; y++) for (INT32 x = 0; x < 4; x++) {
        if (!gTetrisPieces[Piece][Rotation][y][x]) continue;
        INT32 BX = PX + x;
        INT32 BY = PY + y;
        if (BX < 0 || BX >= TETRIS_W || BY >= TETRIS_H) return TRUE;
        if (BY >= 0 && gTetrisBoard[BY][BX]) return TRUE;
    }
    return FALSE;
}

STATIC VOID TetrisMove(INT32 DX) {
    if (gTetrisGameOver) return;
    if (!TetrisCollision(gTetrisX+DX,gTetrisY,gTetrisPiece,gTetrisRotation)) gTetrisX += DX;
}

STATIC VOID TetrisRotate(VOID) {
    if (gTetrisGameOver) return;
    UINT8 NewRotation = (UINT8)((gTetrisRotation + 1) & 3);
    if (!TetrisCollision(gTetrisX,gTetrisY,gTetrisPiece,NewRotation)) gTetrisRotation = NewRotation;
}

STATIC VOID TetrisLockPiece(VOID) {
    for (INT32 y = 0; y < 4; y++) for (INT32 x = 0; x < 4; x++) {
        if (!gTetrisPieces[gTetrisPiece][gTetrisRotation][y][x]) continue;
        INT32 BX = gTetrisX + x;
        INT32 BY = gTetrisY + y;
        if (BX >= 0 && BX < TETRIS_W && BY >= 0 && BY < TETRIS_H)
            gTetrisBoard[BY][BX] = gTetrisPiece + 1;
    }
}

STATIC VOID TetrisClearLines(VOID) {
    UINT32 Cleared = 0;
    for (INT32 y = TETRIS_H - 1; y >= 0; y--) {
        BOOLEAN Full = TRUE;
        for (INT32 x = 0; x < TETRIS_W; x++) if (!gTetrisBoard[y][x]) { Full = FALSE; break; }
        if (Full) {
            for (INT32 yy = y; yy > 0; yy--)
                for (INT32 x = 0; x < TETRIS_W; x++) gTetrisBoard[yy][x] = gTetrisBoard[yy-1][x];
            for (INT32 x = 0; x < TETRIS_W; x++) gTetrisBoard[0][x] = 0;
            Cleared++;
            y++;
        }
    }
    if (Cleared) {
        gTetrisLines += Cleared;
        switch (Cleared) { case 1: gTetrisScore+=100; break; case 2: gTetrisScore+=300; break;
                           case 3: gTetrisScore+=500; break; case 4: gTetrisScore+=800; break; }
        gTetrisLevel = 1 + (gTetrisLines / 10);
    }
}

STATIC VOID TetrisSpawn(VOID) {
    gTetrisPiece = gTetrisNextPiece;
    gTetrisNextPiece = (UINT8)(TetrisRandom() % 7);
    gTetrisX = 3;
    gTetrisY = 0;
    gTetrisRotation = 0;
    if (TetrisCollision(gTetrisX,gTetrisY,gTetrisPiece,gTetrisRotation)) gTetrisGameOver = TRUE;
}

VOID ResetTetris(VOID) {
    EFI_TIME TetrisTime;
    gBS->SetMem(gTetrisBoard, sizeof(gTetrisBoard), 0);
    gTetrisScore = 0;
    gTetrisLines = 0;
    gTetrisLevel = 1;
    gTetrisGameOver = FALSE;
    if (!EFI_ERROR(gRT->GetTime(&TetrisTime, NULL))) gTetrisLastSecond = TetrisTime.Second;
    else gTetrisLastSecond = 0;
    gTetrisPiece = (UINT8)(TetrisRandom() % 7);
    gTetrisNextPiece = (UINT8)(TetrisRandom() % 7);
    gTetrisX = 3;
    gTetrisY = 0;
    gTetrisRotation = 0;
}

VOID UpdateTetris(VOID) {
    if (gTetrisGameOver) return;
    if (!TetrisCollision(gTetrisX,gTetrisY+1,gTetrisPiece,gTetrisRotation)) gTetrisY++;
    else { TetrisLockPiece(); TetrisClearLines(); TetrisSpawn(); }
}

VOID RenderTetris(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 38;
    CHAR16 Buf[64];
    UnicodeSPrint(Buf,sizeof(Buf),L"Score: %d",gTetrisScore);
    DrawString(Buf,X,Y,COLOR_TEXT);
    UnicodeSPrint(Buf,sizeof(Buf),L"Lines: %d",gTetrisLines);
    DrawString(Buf,X+100,Y,COLOR_TEXT);
    UnicodeSPrint(Buf,sizeof(Buf),L"Level: %d",gTetrisLevel);
    DrawString(Buf,X+200,Y,COLOR_TEXT);
    INT32 Cell = 20;
    INT32 BoardX = X;
    INT32 BoardY = Y + 20;
    DrawRect(BoardX-2,BoardY-2,TETRIS_W*Cell+4,TETRIS_H*Cell+4,COLOR_WIN_BORDER);
    for (INT32 y = 0; y < TETRIS_H; y++) for (INT32 x = 0; x < TETRIS_W; x++)
        DrawRect(BoardX+x*Cell,BoardY+y*Cell,Cell-1,Cell-1,TetrisCellColor(gTetrisBoard[y][x]));
    if (!gTetrisGameOver) {
        for (INT32 y = 0; y < 4; y++) for (INT32 x = 0; x < 4; x++) {
            if (!gTetrisPieces[gTetrisPiece][gTetrisRotation][y][x]) continue;
            INT32 BX = gTetrisX + x;
            INT32 BY = gTetrisY + y;
            if (BX >= 0 && BX < TETRIS_W && BY >= 0 && BY < TETRIS_H)
                DrawRect(BoardX+BX*Cell,BoardY+BY*Cell,Cell-1,Cell-1,TetrisCellColor(gTetrisPiece+1));
        }
    }
    INT32 SideX = BoardX + TETRIS_W*Cell + 18;
    DrawString(L"NEXT",SideX,BoardY,COLOR_TEXT);
    for (INT32 y = 0; y < 4; y++) for (INT32 x = 0; x < 4; x++)
        if (gTetrisPieces[gTetrisNextPiece][0][y][x])
            DrawRect(SideX+x*16,BoardY+18+y*16,15,15,TetrisCellColor(gTetrisNextPiece+1));
    DrawString(L"LEFT / RIGHT",SideX,BoardY+100,COLOR_TEXT);
    DrawString(L"UP = ROTATE",SideX,BoardY+116,COLOR_TEXT);
    DrawString(L"DOWN = DROP",SideX,BoardY+132,COLOR_TEXT);
    DrawButton(X,Win->Y+Win->H-34,80,24,L"Restart",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    if (gTetrisGameOver) DrawString(L"GAME OVER",BoardX+42,BoardY+180,RGB(239,68,68));
}

BOOLEAN InputTetris(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    if (Click && !Last && IsInRect(MX,MY,Win->X+12,Win->Y+Win->H-34,80,24)) ResetTetris();
    return TRUE;
}

VOID RenderImageViewer(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;
    DrawButton(X,Y,65,22,L"Open",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(X+70,Y,65,22,L"1x",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+140,Y,65,22,L"2x",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawString(gImageViewerFileName,X+220,Y+7,COLOR_TEXT);
    INT32 FrameX = X + 10;
    INT32 FrameY = Y + 34;
    DrawRect(FrameX-2,FrameY-2,PAINT_W+4,PAINT_H+4,COLOR_WIN_BORDER);
    if (!gImageViewerLoaded) {
        DrawString(L"No image loaded.",FrameX+65,FrameY+80,RGB(148,163,184));
        DrawString(L"Open a .pnt file.",FrameX+65,FrameY+100,RGB(100,116,139));
        return;
    }
    for (INT32 py = 0; py < PAINT_H; py++) for (INT32 px = 0; px < PAINT_W; px++) {
        UINT32 C = gImageViewerCanvas[py*PAINT_W+px];
        if (gImageViewerZoom == 1) FastDrawPixel(FrameX+px,FrameY+py,C);
        else DrawRect(FrameX+px*2,FrameY+py*2,2,2,C);
    }
}

BOOLEAN InputImageViewer(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 8;
    INT32 Y = Win->Y + 34;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X,Y,65,22)) {
        FileDialogOpen(FILEDLG_OPEN, FD_P_IMAGE_LOAD);
        return TRUE;
    }
    if (IsInRect(MX,MY,X+70,Y,65,22)) gImageViewerZoom = 1;
    if (IsInRect(MX,MY,X+140,Y,65,22)) gImageViewerZoom = 2;
    return TRUE;
}

STATIC BOOLEAN IsLeapYear(UINT16 Year) {
    return (((Year % 4) == 0 && (Year % 100) != 0) || ((Year % 400) == 0));
}

STATIC UINT8 DaysInMonth(UINT16 Year, UINT8 Month) {
    STATIC CONST UINT8 Days[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (Month == 2 && IsLeapYear(Year)) return 29;
    if (Month >= 1 && Month <= 12) return Days[Month - 1];
    return 30;
}

VOID RenderCommonApps(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    DrawButton(X,Y,82,24,L"Calendar",gCommonAppTab==COMMON_APP_CALENDAR?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+86,Y,82,24,L"Clock",gCommonAppTab==COMMON_APP_CLOCK?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+172,Y,82,24,L"Stopwatch",gCommonAppTab==COMMON_APP_STOPWATCH?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+258,Y,82,24,L"About",gCommonAppTab==COMMON_APP_ABOUT?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);
    INT32 ContentY = Y + 38;
    DrawRect(X,ContentY,Win->W-20,Win->H-86,COLOR_CANVAS);
    if (gCommonAppTab == COMMON_APP_CALENDAR) {
        EFI_TIME Time;
        if (!gCalendarInitialized) {
            if (!EFI_ERROR(gRT->GetTime(&Time, NULL))) {
                gCalendarYear = Time.Year; gCalendarMonth = Time.Month; gCalendarSelectedDay = Time.Day;
            }
            gCalendarInitialized = TRUE;
        }
        CHAR16 Header[64];
        UnicodeSPrint(Header,sizeof(Header),L"%04d / %02d",gCalendarYear,gCalendarMonth);
        DrawString(Header,X+90,ContentY+8,COLOR_TEXT);
        DrawButton(X+10,ContentY+4,55,22,L"<",COLOR_TOOLBAR_BG,COLOR_TEXT);
        DrawButton(X+230,ContentY+4,55,22,L">",COLOR_TOOLBAR_BG,COLOR_TEXT);
        CONST CHAR16 *Days[] = { L"Su",L"Mo",L"Tu",L"We",L"Th",L"Fr",L"Sa" };
        INT32 GridX = X + 12;
        INT32 GridY = ContentY + 38;
        for (INT32 i = 0; i < 7; i++) DrawString(Days[i],GridX+i*36,GridY,RGB(148,163,184));
        UINT16 YR = gCalendarYear;
        UINT8 MO = gCalendarMonth;
        UINT16 yy = YR;
        UINT8 mm = MO;
        if (mm < 3) { mm += 12; yy--; }
        INT32 WDay = (1 + (13 * (mm + 1)) / 5 + yy + yy/4 - yy/100 + yy/400) % 7;
        WDay = (WDay + 6) % 7;
        UINT8 MonthDays = DaysInMonth(gCalendarYear, gCalendarMonth);
        for (UINT8 Day = 1; Day <= MonthDays; Day++) {
            INT32 Pos = WDay + Day - 1;
            INT32 Col = Pos % 7;
            INT32 Row = Pos / 7;
            INT32 DX = GridX + Col * 36;
            INT32 DY = GridY + 18 + Row * 25;
            UINT32 BG = (Day == gCalendarSelectedDay) ? COLOR_TITLE_ACTIVE : COLOR_TOOLBAR_BG;
            DrawRect(DX,DY,28,20,BG);
            CHAR16 DayBuf[8];
            UnicodeSPrint(DayBuf,sizeof(DayBuf),L"%d",Day);
            DrawString(DayBuf,DX+10,DY+6,COLOR_TEXT);
        }
    } else if (gCommonAppTab == COMMON_APP_CLOCK) {
        EFI_TIME Time;
        if (!EFI_ERROR(gRT->GetTime(&Time, NULL))) {
            CHAR16 Buf[32];
            UnicodeSPrint(Buf,sizeof(Buf),L"%02d:%02d:%02d",Time.Hour,Time.Minute,Time.Second);
            DrawString(Buf,X+35,ContentY+45,NEW_APP_COLOR_GOOD);
            UnicodeSPrint(Buf,sizeof(Buf),L"%02d/%02d/%04d",Time.Day,Time.Month,Time.Year);
            DrawString(Buf,X+75,ContentY+75,COLOR_TEXT);
            DrawString(L"UEFI Real-Time Clock",X+72,ContentY+105,RGB(148,163,184));
        }
    } else if (gCommonAppTab == COMMON_APP_STOPWATCH) {
        CHAR16 Buf[32];
        UINT32 Seconds = gStopwatchTicks;
        UINT32 Minutes = Seconds / 60;
        Seconds %= 60;
        UnicodeSPrint(Buf,sizeof(Buf),L"%02d:%02d",Minutes,Seconds);
        DrawString(Buf,X+30,ContentY+20,NEW_APP_COLOR_GOOD);
        DrawButton(X+20,ContentY+60,80,24,gStopwatchRunning?L"Stop":L"Start",
            gStopwatchRunning?COLOR_BTN_CLOSE:NEW_APP_COLOR_GOOD,COLOR_TEXT);
        DrawButton(X+108,ContentY+60,80,24,L"Reset",COLOR_WIN_BORDER,COLOR_TEXT);
    } else {
        DrawString(L"UEFIText",X+12,ContentY+12,NEW_APP_COLOR_ACCENT);
        DrawString(L"Native UEFI desktop environment",X+12,ContentY+36,COLOR_TEXT);
        DrawString(L"x64 / GOP / UEFI",X+12,ContentY+54,RGB(148,163,184));
        DrawString(L"Made in UEFI C",X+12,ContentY+72,RGB(148,163,184));
        DrawString(L"Version 1.0",X+12,ContentY+90,RGB(148,163,184));
    }
}

BOOLEAN InputCommonApps(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X,Y,82,24)) { gCommonAppTab = COMMON_APP_CALENDAR; return TRUE; }
    if (IsInRect(MX,MY,X+86,Y,82,24)) { gCommonAppTab = COMMON_APP_CLOCK; return TRUE; }
    if (IsInRect(MX,MY,X+172,Y,82,24)) { gCommonAppTab = COMMON_APP_STOPWATCH; return TRUE; }
    if (IsInRect(MX,MY,X+258,Y,82,24)) { gCommonAppTab = COMMON_APP_ABOUT; return TRUE; }
    if (gCommonAppTab == COMMON_APP_CALENDAR) {
        INT32 ContentY = Y + 38;
        if (IsInRect(MX,MY,X+10,ContentY+4,55,22)) {
            if (gCalendarMonth == 1) { gCalendarMonth = 12; gCalendarYear--; } else gCalendarMonth--;
            UINT8 MaxDay = DaysInMonth(gCalendarYear,gCalendarMonth);
            if (gCalendarSelectedDay > MaxDay) gCalendarSelectedDay = MaxDay;
            return TRUE;
        }
        if (IsInRect(MX,MY,X+230,ContentY+4,55,22)) {
            if (gCalendarMonth == 12) { gCalendarMonth = 1; gCalendarYear++; } else gCalendarMonth++;
            UINT8 MaxDay = DaysInMonth(gCalendarYear,gCalendarMonth);
            if (gCalendarSelectedDay > MaxDay) gCalendarSelectedDay = MaxDay;
            return TRUE;
        }
    }
    if (gCommonAppTab == COMMON_APP_STOPWATCH) {
        INT32 ContentY = Y + 38;
        if (IsInRect(MX,MY,X+20,ContentY+60,80,24)) {
            gStopwatchRunning = !gStopwatchRunning;
            if (gStopwatchRunning) {
                EFI_TIME StartTime;
                if (!EFI_ERROR(gRT->GetTime(&StartTime,NULL))) gStopwatchLastSecond = StartTime.Second;
            }
            return TRUE;
        }
        if (IsInRect(MX,MY,X+108,ContentY+60,80,24)) {
            gStopwatchTicks = 0; gStopwatchRunning = FALSE;
            EFI_TIME ResetTime;
            if (!EFI_ERROR(gRT->GetTime(&ResetTime,NULL))) gStopwatchLastSecond = ResetTime.Second;
            return TRUE;
        }
    }
    return TRUE;
}

STATIC VOID ClipboardAdd(CONST CHAR16 *Text) {
    if (!Text || Text[0] == L'\0') return;
    if (gClipboardCount >= CLIPBOARD_HISTORY) {
        for (UINTN i = 0; i < CLIPBOARD_HISTORY - 1; i++)
            gBS->CopyMem(gClipboardHistory[i],gClipboardHistory[i+1],sizeof(gClipboardHistory[i]));
        gClipboardCount = CLIPBOARD_HISTORY - 1;
    }
    StrCpyS(gClipboardHistory[gClipboardCount],CLIPBOARD_TEXT_MAX,Text);
    gClipboardCount++;
    gClipboardSelected = (INT32)gClipboardCount - 1;
}

VOID RenderClipboard(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    DrawButton(X,Y,120,24,L"Copy Editor",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(X+125,Y,120,24,L"Copy Notes",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+250,Y,80,24,L"Clear",COLOR_BTN_CLOSE,COLOR_TEXT);
    DrawString(L"Clipboard History",X,Y+40,COLOR_TEXT);
    for (UINTN i = 0; i < gClipboardCount; i++) {
        INT32 RowY = Y + 60 + (INT32)i * 28;
        UINT32 BG = ((INT32)i == gClipboardSelected) ? COLOR_TITLE_ACTIVE : COLOR_TOOLBAR_BG;
        DrawRect(X,RowY,Win->W-20,23,BG);
        DrawString(gClipboardHistory[i],X+6,RowY+7,COLOR_TEXT);
    }
}

BOOLEAN InputClipboard(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X,Y,120,24)) { ClipboardAdd(gTextBuffer); return TRUE; }
    if (IsInRect(MX,MY,X+125,Y,120,24)) { ClipboardAdd(gNotesBuffer); return TRUE; }
    if (IsInRect(MX,MY,X+250,Y,80,24)) { gClipboardCount = 0; gClipboardSelected = -1; return TRUE; }
    for (UINTN i = 0; i < gClipboardCount; i++) {
        INT32 RowY = Y + 60 + (INT32)i * 28;
        if (IsInRect(MX,MY,X,RowY,Win->W-20,23)) { gClipboardSelected = (INT32)i; return TRUE; }
    }
    return TRUE;
}

VOID RenderUnitConverter(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    DrawButton(X,Y,100,24,L"Bytes",gUnitMode==UNIT_BYTES?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(X+105,Y,100,24,L"Temp",gUnitMode==UNIT_TEMPERATURE?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawRect(X,Y+40,220,26,COLOR_CANVAS);
    DrawString(gUnitInput,X+8,Y+48,COLOR_TEXT);
    if (gUnitMode == UNIT_BYTES) {
        UINT64 Value = StrDecimalToUintn(gUnitInput);
        CHAR16 Buf[64];
        UnicodeSPrint(Buf,sizeof(Buf),L"%d bytes",(UINT32)Value); DrawString(Buf,X,Y+82,COLOR_TEXT);
        UnicodeSPrint(Buf,sizeof(Buf),L"%d KB",(UINT32)(Value/1024)); DrawString(Buf,X,Y+100,NEW_APP_COLOR_ACCENT);
        UnicodeSPrint(Buf,sizeof(Buf),L"%d MB",(UINT32)(Value/(1024*1024))); DrawString(Buf,X,Y+118,NEW_APP_COLOR_GOOD);
        UnicodeSPrint(Buf,sizeof(Buf),L"%d GB",(UINT32)(Value/(1024*1024*1024))); DrawString(Buf,X,Y+136,NEW_APP_COLOR_WARN);
    } else {
        INT32 C = (INT32)StrDecimalToUintn(gUnitInput);
        INT32 F = (C * 9) / 5 + 32;
        INT32 BackC = ((C - 32) * 5) / 9;
        CHAR16 Buf[64];
        UnicodeSPrint(Buf,sizeof(Buf),L"%d C = %d F",C,F); DrawString(Buf,X,Y+86,COLOR_TEXT);
        UnicodeSPrint(Buf,sizeof(Buf),L"%d F -> %d C",C,BackC); DrawString(Buf,X,Y+108,NEW_APP_COLOR_ACCENT);
    }
    DrawString(L"Type number with keyboard.",X,Y+170,RGB(100,116,139));
}

BOOLEAN InputUnitConverter(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X,Y,100,24)) { gUnitMode = UNIT_BYTES; return TRUE; }
    if (IsInRect(MX,MY,X+105,Y,100,24)) { gUnitMode = UNIT_TEMPERATURE; return TRUE; }
    return TRUE;
}

STATIC BOOLEAN SearchContains(CONST CHAR16 *Text, CONST CHAR16 *Query) {
    if (!Query || Query[0] == L'\0') return FALSE;
    for (UINTN i = 0; Text[i]; i++) {
        UINTN j = 0;
        while (Text[i+j] && Query[j] && Text[i+j] == Query[j]) j++;
        if (Query[j] == L'\0') return TRUE;
    }
    return FALSE;
}

STATIC VOID ExecuteFileSearch(VOID) {
    RefreshFileList();
    gSearchResultCount = 0;
    for (UINTN i = 0; i < gFileCount && gSearchResultCount < MAX_FILES; i++) {
        if (SearchContains(gFileList[i],gSearchQuery)) {
            StrCpyS(gSearchResults[gSearchResultCount],64,gFileList[i]);
            gSearchResultCount++;
        }
    }
    gSearchDone = TRUE;
}

VOID RenderSearch(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    DrawRect(X,Y,360,26,COLOR_CANVAS);
    DrawString(gSearchQuery,X+7,Y+8,COLOR_TEXT);
    DrawButton(X+370,Y,80,26,L"Search",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawString(gSearchDone?L"Results:":L"Enter filename to search",X,Y+42,COLOR_TEXT);
    for (UINTN i = 0; i < gSearchResultCount; i++) {
        DrawRect(X,Y+62+(INT32)i*22,360,20,COLOR_TOOLBAR_BG);
        DrawString(gSearchResults[i],X+6,Y+68+(INT32)i*22,COLOR_TEXT);
    }
}

BOOLEAN InputSearch(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 10;
    INT32 Y = Win->Y + 36;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X+370,Y,80,26)) { ExecuteFileSearch(); return TRUE; }
    return TRUE;
}

typedef struct { UINT32 Frequency; UINT32 Duration; } MUSIC_NOTE;
#include "scarlet.h"
STATIC CONST MUSIC_NOTE gMusicSong0[] = {
    {523,120},{659,120},{784,120},{1047,220},{784,120},{659,120},{523,220},{0,80},
    {659,120},{784,120},{1047,180},{988,120},{784,120},{659,220},{0,100}
};
STATIC UINTN   gScarletIdx = 0;
STATIC UINT64  gScarletNoteEndTsc = 0;
STATIC BOOLEAN gScarletPlaying = FALSE;

VOID PlayMusicSong(UINTN Song) {
    if (Song == 0) {
        UINTN Count = sizeof(gMusicSong0) / sizeof(gMusicSong0[0]);
        gMusicPlaying = TRUE;
        for (UINTN i = 0; i < Count; i++) PlayTone(gMusicSong0[i].Frequency,gMusicSong0[i].Duration);
        gMusicPlaying = FALSE;
    }
}

VOID RenderMusic(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 40;
    DrawString(L"PC Speaker Music",X,Y,COLOR_TEXT);
    DrawString(L"Tiny UEFIText melody",X,Y+20,RGB(148,163,184));
    DrawButton(X,Y+55,130,28,L"Play Jingle",COLOR_TITLE_ACTIVE,COLOR_TEXT);
    DrawButton(X+140,Y+55,130,28,L"Scarlet Fire",NEW_APP_COLOR_GOOD,COLOR_TEXT);
    DrawButton(X+280,Y+55,100,28,L"Stop",COLOR_BTN_CLOSE,COLOR_TEXT);
    DrawString(gMusicPlaying?L"Playing jingle...":
               (gScarletPlaying?L"Scarlet Fire playing (async)":L"Ready."),
               X,Y+100,(gMusicPlaying||gScarletPlaying)?NEW_APP_COLOR_GOOD:COLOR_TEXT);
    DrawString(L"Uses the PC speaker through PIT.",X,Y+135,RGB(100,116,139));
    DrawString(L"Scarlet Fire plays in the background.",X,Y+150,RGB(100,116,139));
}

BOOLEAN InputMusic(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    INT32 X = Win->X + 12;
    INT32 Y = Win->Y + 40;
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,X,Y+55,130,28)) { PlayMusicSong(0); return TRUE; }
    if (IsInRect(MX,MY,X+140,Y+55,130,28)) { ScarletFireStart(); return TRUE; }
    if (IsInRect(MX,MY,X+280,Y+55,100,28)) { ScarletFireStop(); return TRUE; }
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
                gRT->ResetSystem(EfiResetShutdown,EFI_SUCCESS,0,NULL);
            }
        }
        return;
    }
    if (!gJumpscareActive) return;
    gJumpscareElapsedMs += 10;
    if ((JumpscareRandom() % 7) == 0) gJumpscareSoundPending = TRUE;
    if (gJumpscareElapsedMs >= 3000) {
        gJumpscareActive = FALSE;
        gJumpscareSoundPending = FALSE;
        gJumpscareMessage = TRUE;
        gJumpscareMessageTimer = 150;
    }
}

STATIC VOID PlayJumpscareBeep(VOID) {
    UINT32 Frequency;
    switch (JumpscareRandom() % 8) {
        case 0: Frequency = 80; break;
        case 1: Frequency = 130; break;
        case 2: Frequency = 220; break;
        case 3: Frequency = 440; break;
        case 4: Frequency = 660; break;
        case 5: Frequency = 900; break;
        case 6: Frequency = 1400; break;
        default: Frequency = 1900; break;
    }
    UINT32 Duration = 15 + (JumpscareRandom() % 70);
    PlayTone(Frequency,Duration);
}

STATIC VOID RenderJumpscare(VOID) {
    INT32 TileW = 16;
    INT32 TileH = 16;
    for (INT32 Y = 0; Y < (INT32)gScreenHeight; Y += TileH)
        for (INT32 X = 0; X < (INT32)gScreenWidth; X += TileW) {
            UINT32 R = JumpscareRandom() & 0xFF;
            UINT32 G = JumpscareRandom() & 0xFF;
            UINT32 B = JumpscareRandom() & 0xFF;
            UINT32 Mode = JumpscareRandom() % 6;
            if (Mode == 0) { R=0; G=0; B=0; }
            else if (Mode == 1) { R=255; G=255; B=255; }
            else if (Mode == 2) { R=255; G=0; B=0; }
            else if (Mode == 3) { R=0; G=255; B=0; }
            else if (Mode == 4) { R=0; G=0; B=255; }
            DrawRect(X,Y,TileW,TileH,RGB(R,G,B));
        }
    for (INT32 i = 0; i < 24; i++) {
        INT32 Y = (INT32)(JumpscareRandom() % gScreenHeight);
        INT32 H = 1 + (INT32)(JumpscareRandom() % 14);
        INT32 X = (INT32)(JumpscareRandom() % gScreenWidth);
        INT32 W = 20 + (INT32)(JumpscareRandom() % (gScreenWidth / 2));
        UINT32 Color = RGB(JumpscareRandom()&0xFF,JumpscareRandom()&0xFF,JumpscareRandom()&0xFF);
        DrawRect(X,Y,W,H,Color);
    }
    if ((JumpscareRandom() % 9) == 0) {
        UINT32 FlashColor;
        switch (JumpscareRandom() % 5) {
            case 0: FlashColor = RGB(255,0,0); break;
            case 1: FlashColor = RGB(0,255,0); break;
            case 2: FlashColor = RGB(0,0,255); break;
            case 3: FlashColor = RGB(255,255,255); break;
            default: FlashColor = RGB(0,0,0); break;
        }
        DrawRect(0,0,gScreenWidth,gScreenHeight,FlashColor);
    }
}

STATIC VOID RenderJumpscareMessage(VOID) {
    DrawRect(0,0,gScreenWidth,gScreenHeight,RGB(0,0,0));
    INT32 X = 20;
    INT32 Y = 30;
    DrawString(L"UEFIText secret message:",X,Y,COLOR_TEXT);
    Y += 24;
    UINTN Count = sizeof(gJumpscareHexMessage) / sizeof(gJumpscareHexMessage[0]);
    for (UINTN i = 0; i < Count; i++) { DrawString(gJumpscareHexMessage[i],X,Y,RGB(100,255,100)); Y += 12; }
    DrawString(L"bye bye but the text is base16 hope it helps ;D n' ._.",X,Y+12,RGB(100,150,150));
}

VOID RenderEasterEgg(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 20;
    INT32 Y = Win->Y + 44;
    DrawString(L"CONGRATULATIONS.",X,Y,NEW_APP_COLOR_GOOD);
    DrawString(L"You found the forbidden UEFIText room.",X,Y+24,COLOR_TEXT);
    DrawString(L"._.",X+150,Y+65,RGB(255,255,255));
    DrawString(L"Nothing important is here.",X,Y+105,RGB(100,116,139));
    DrawString(L"...probably.",X,Y+125,RGB(100,116,139));
    DrawString(L"maybe do something else :D",X,Y+95,RGB(255,155,255));
    DrawButton(X+105,Y+160,100,25,L"Leave",COLOR_BTN_CLOSE,COLOR_TEXT);
}

BOOLEAN InputEasterEgg(UEFI_WINDOW *Win,INT32 MX,INT32 MY,BOOLEAN Click,BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    if (IsInRect(MX,MY,Win->X+125,Win->Y+44+160,100,25)) {
        gEasterEggWin.IsVisible = FALSE;
        StartJumpscare();
        return TRUE;
    }
    return TRUE;
}

STATIC CONST UINT32 gAccentPaletteA[32] = {
    /* row 1 - blues / cyans */
    RGB(37,99,235),   RGB(59,130,246),  RGB(96,165,250),  RGB(147,197,253),
    RGB(8,145,178),   RGB(14,165,233),  RGB(56,189,248),  RGB(125,211,252),
    /* row 2 - greens */
    RGB(22,163,74),   RGB(34,197,94),   RGB(74,222,128),  RGB(134,239,172),
    RGB(16,185,129),  RGB(52,211,153),  RGB(110,231,183), RGB(167,243,208),
    /* row 3 - purples / magentas */
    RGB(124,58,237),  RGB(139,92,246),  RGB(167,139,250), RGB(196,181,253),
    RGB(192,38,211),  RGB(217,70,239),  RGB(232,121,249), RGB(240,171,252),
    /* row 4 - warm */
    RGB(220,38,38),   RGB(239,68,68),   RGB(248,113,113), RGB(252,165,165),
    RGB(234,88,12),   RGB(249,115,22),  RGB(251,146,60),  RGB(253,186,116)
};

STATIC CONST UINT32 gAccentPaletteB[32] = {
    /* row 1 - neutrals */
    RGB(15,23,42),    RGB(30,41,59),    RGB(51,65,85),    RGB(71,85,105),
    RGB(100,116,139), RGB(148,163,184), RGB(203,213,225), RGB(226,232,240),
    /* row 2 - pastels */
    RGB(254,202,202), RGB(253,230,138), RGB(187,247,208), RGB(191,219,254),
    RGB(221,214,254), RGB(251,207,232), RGB(254,215,170), RGB(204,251,241),
    /* row 3 - deep / rich */
    RGB(127,29,29),   RGB(120,53,15),   RGB(133,77,14),   RGB(63,98,18),
    RGB(20,83,45),    RGB(19,78,74),    RGB(12,74,110),   RGB(30,58,138),
    /* row 4 - vivid */
    RGB(255,0,0),     RGB(255,128,0),   RGB(255,255,0),   RGB(0,255,0),
    RGB(0,255,255),   RGB(0,128,255),   RGB(128,0,255),   RGB(255,0,255)
};

STATIC VOID DrawPaletteGrid(INT32 X, INT32 Y, CONST UINT32 *Palette, UINT32 Selected) {
    for (INT32 i = 0; i < 32; i++) {
        INT32 bx = X + (i % 8) * 48;
        INT32 by = Y + (i / 8) * 22;
        DrawRect(bx, by, 44, 18, Palette[i]);
        if (Selected == Palette[i]) {
            DrawRect(bx - 2, by - 2,  48, 2, COLOR_TEXT);
            DrawRect(bx - 2, by + 18, 48, 2, COLOR_TEXT);
            DrawRect(bx - 2, by,       2, 20, COLOR_TEXT);
            DrawRect(bx + 44, by,      2, 20, COLOR_TEXT);
        }
    }
}

VOID RenderSettings(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawButton(X,     Y, 130, 24, L"Appearance", gSettingSection==0?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+135, Y, 100, 24, L"Fonts",      gSettingSection==1?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+240, Y, 120, 24, L"System",     gSettingSection==2?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
    INT32 CY = Y + 40;
    if (gSettingSection == 0) {
        DrawString(L"Wallpaper Mode", X, CY, COLOR_TEXT);
        DrawButton(X,     CY+20, 88, 22, L"Solid",   gWallpaperMode==0?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+92,  CY+20, 88, 22, L"V-Grad",  gWallpaperMode==1?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+184, CY+20, 88, 22, L"H-Grad",  gWallpaperMode==2?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+276, CY+20, 88, 22, L"Checker", gWallpaperMode==3?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+368, CY+20, 88, 22, L"Stripes", gWallpaperMode==4?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X,     CY+44, 88, 22, L"Diag",    gWallpaperMode==5?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+92,  CY+44, 88, 22, L"Dots",    gWallpaperMode==6?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+184, CY+44, 88, 22, L"Radial",  gWallpaperMode==7?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+276, CY+44, 88, 22, L"Waves",   gWallpaperMode==8?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+368, CY+44, 88, 22, L"Plasma",  gWallpaperMode==9?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X,     CY+68, 88, 22, L"Scroll",  gWallpaperMode==10?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawString(L"Accent A", X, CY+100, COLOR_TEXT);
        DrawPaletteGrid(X, CY+118, gAccentPaletteA, gWallpaperColor1);
        DrawString(L"Accent B", X, CY+220, COLOR_TEXT);
        DrawPaletteGrid(X, CY+238, gAccentPaletteB, gWallpaperColor2);
        DrawButton(X, CY+350, 130, 24, gWallpaperAnimate?L"Animate: ON":L"Animate: OFF",
            gWallpaperAnimate?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG, COLOR_TEXT);
    }
    else if (gSettingSection == 1) {
        DrawString(L"Font Style", X, CY, COLOR_TEXT);
        DrawButton(X,     CY+20, 96, 24, L"Sans",  gFontStyle==FONT_STYLE_SANS ?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+100, CY+20, 96, 24, L"Bold",  gFontStyle==FONT_STYLE_BOLD ?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+200, CY+20, 96, 24, L"Mono",  gFontStyle==FONT_STYLE_MONO ?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+300, CY+20, 96, 24, L"Comic", gFontStyle==FONT_STYLE_COMIC?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawString(L"Scale", X, CY+58, COLOR_TEXT);
        DrawButton(X,     CY+76, 60, 24, L"1x", gFontScale==1?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+64,  CY+76, 60, 24, L"2x", gFontScale==2?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+128, CY+76, 60, 24, L"3x", gFontScale==3?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawString(L"Preview of the selected font:", X, CY+118, COLOR_TEXT);
        DrawString(L"Hello World! 0123456789 !@#$%^&*()", X, CY+140, NEW_APP_COLOR_ACCENT);
    }
    else {
        DrawString(L"Clock Format", X, CY, COLOR_TEXT);
        DrawButton(X,     CY+20, 80, 24, L"12h", gSettingClock24h?COLOR_TOOLBAR_BG:COLOR_TITLE_ACTIVE, COLOR_TEXT);
        DrawButton(X+84,  CY+20, 80, 24, L"24h", gSettingClock24h?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawString(L"Mouse Speed", X, CY+58, COLOR_TEXT);
        DrawButton(X,     CY+76, 80, 24, L"Slow",   gSettingMouseSpeed==0?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+84,  CY+76, 80, 24, L"Normal", gSettingMouseSpeed==1?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+168, CY+76, 80, 24, L"Fast",   gSettingMouseSpeed==2?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);

        DrawString(L"Theme Preset", X, CY+114, COLOR_TEXT);
        DrawButton(X,      CY+132, 110, 24, L"Slate",   COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+120,  CY+132, 110, 24, L"Nord",    COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+240,  CY+132, 110, 24, L"Dracula", COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+360,  CY+132, 110, 24, L"Paper",   COLOR_TOOLBAR_BG, COLOR_TEXT);

        DrawButton(X,      CY+210, 100, 24, L"Sound",  gSoundEnabled?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG, COLOR_TEXT);
        DrawButton(X+220,  CY+210, 100, 24, L"Reset",      COLOR_BTN_CLOSE,    COLOR_TEXT);

        DrawString(L"Power", X, CY+244, COLOR_TEXT);
        DrawButton(X,      CY+262, 100, 24, L"Shell :>",    NEW_APP_COLOR_GOOD, COLOR_TEXT);
        DrawButton(X+110,  CY+262, 100, 24, L"Reboot",   COLOR_TOOLBAR_BG,   COLOR_TEXT);
        DrawButton(X+220,  CY+262, 100, 24, L"Shutdown", COLOR_BTN_CLOSE,    COLOR_TEXT);
    }
}

BOOLEAN InputSettings(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36, CY = Y + 40;
    if (IsInRect(MX,MY,X,Y,130,24)) { gSettingSection = 0; return TRUE; }
    if (IsInRect(MX,MY,X+135,Y,100,24)) { gSettingSection = 1; return TRUE; }
    if (IsInRect(MX,MY,X+240,Y,120,24)) { gSettingSection = 2; return TRUE; }
    if (gSettingSection == 0) {
        if (IsInRect(MX,MY,X,     CY+20, 88,22)) { gWallpaperMode=0;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+92,  CY+20, 88,22)) { gWallpaperMode=1;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+184, CY+20, 88,22)) { gWallpaperMode=2;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+276, CY+20, 88,22)) { gWallpaperMode=3;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+368, CY+20, 88,22)) { gWallpaperMode=4;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X,     CY+44, 88,22)) { gWallpaperMode=5;  gWallpaperCacheDirty=TRUE; gWallpaperAnimate=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+92,  CY+44, 88,22)) { gWallpaperMode=6;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+184, CY+44, 88,22)) { gWallpaperMode=7;  gWallpaperCacheDirty=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+276, CY+44, 88,22)) { gWallpaperMode=8;  gWallpaperCacheDirty=TRUE; gWallpaperAnimate=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+368, CY+44, 88,22)) { gWallpaperMode=9;  gWallpaperCacheDirty=TRUE; gWallpaperAnimate=TRUE; return TRUE; }
        if (IsInRect(MX,MY,X,     CY+68, 88,22)) { gWallpaperMode=10; gWallpaperCacheDirty=TRUE; gWallpaperAnimate=TRUE; return TRUE; }
        for (INT32 i = 0; i < 32; i++) {
            INT32 bx = X + (i % 8) * 48;
            INT32 by = CY + 118 + (i / 8) * 22;
            if (IsInRect(MX,MY,bx,by,44,18)) {
                gWallpaperColor1 = gAccentPaletteA[i];
                gWallpaperCacheDirty = TRUE;
                return TRUE;
            }
        }
        for (INT32 i = 0; i < 32; i++) {
            INT32 bx = X + (i % 8) * 48;
            INT32 by = CY + 238 + (i / 8) * 22;
            if (IsInRect(MX,MY,bx,by,44,18)) {
                gWallpaperColor2 = gAccentPaletteB[i];
                gWallpaperCacheDirty = TRUE;
                return TRUE;
            }
        }
        if (IsInRect(MX,MY,X,     CY+350,130,24)){ gWallpaperAnimate=!gWallpaperAnimate; gWallpaperCacheDirty=TRUE; return TRUE; }
    } else if (gSettingSection == 1) {
        if (IsInRect(MX,MY,X,     CY+20, 96,24)) { gFontStyle=FONT_STYLE_SANS;  return TRUE; }
        if (IsInRect(MX,MY,X+100, CY+20, 96,24)) { gFontStyle=FONT_STYLE_BOLD;  return TRUE; }
        if (IsInRect(MX,MY,X+200, CY+20, 96,24)) { gFontStyle=FONT_STYLE_MONO;  return TRUE; }
        if (IsInRect(MX,MY,X+300, CY+20, 96,24)) { gFontStyle=FONT_STYLE_COMIC; return TRUE; }
        if (IsInRect(MX,MY,X,     CY+76, 60,24)) { gFontScale=1; return TRUE; }
        if (IsInRect(MX,MY,X+64,  CY+76, 60,24)) { gFontScale=2; return TRUE; }
        if (IsInRect(MX,MY,X+128, CY+76, 60,24)) { gFontScale=3; return TRUE; }
    } else {
        if (IsInRect(MX,MY,X,     CY+20, 80,24)) { gSettingClock24h=0; return TRUE; }
        if (IsInRect(MX,MY,X+84,  CY+20, 80,24)) { gSettingClock24h=1; return TRUE; }
        if (IsInRect(MX,MY,X,     CY+76, 80,24)) { gSettingMouseSpeed=0; return TRUE; }
        if (IsInRect(MX,MY,X+84,  CY+76, 80,24)) { gSettingMouseSpeed=1; return TRUE; }
        if (IsInRect(MX,MY,X+168, CY+76, 80,24)) { gSettingMouseSpeed=2; return TRUE; }
        if (IsInRect(MX,MY,X,     CY+132,110,24)) { ApplyThemePreset(0); return TRUE; }
        if (IsInRect(MX,MY,X+120, CY+132,110,24)) { ApplyThemePreset(1); return TRUE; }
        if (IsInRect(MX,MY,X+240, CY+132,110,24)) { ApplyThemePreset(2); return TRUE; }
        if (IsInRect(MX,MY,X+360, CY+132,110,24)) { ApplyThemePreset(3); return TRUE; }
        if (IsInRect(MX,MY,X,     CY+210,100,24)) { gSoundEnabled = !gSoundEnabled; return TRUE; }
        if (IsInRect(MX,MY,X+220, CY+210,100,24)) { ResetDefaults(); return TRUE; }
        if (IsInRect(MX,MY,X,     CY+262,100,24)) { gInShellMode = TRUE; return TRUE; }
        if (IsInRect(MX,MY,X+110, CY+262,100,24)) { ShowShutdownSequence(TRUE);  return TRUE; }
        if (IsInRect(MX,MY,X+220, CY+262,100,24)) { ShowShutdownSequence(FALSE); return TRUE; }
    }
    return TRUE;
}

STATIC UINT32 G2048Rand(VOID) { g2048Rng = g2048Rng*1664525u + 1013904223u; return g2048Rng; }

STATIC VOID G2048Spawn(VOID) {
    INT32 empties[16][2], n = 0;
    for (INT32 y=0;y<G2048_N;y++) for (INT32 x=0;x<G2048_N;x++)
        if (!g2048Board[y][x]) { empties[n][0]=x; empties[n][1]=y; n++; }
    if (!n) return;
    UINT32 r = G2048Rand() % n;
    g2048Board[empties[r][1]][empties[r][0]] = (G2048Rand()%10 < 9) ? 2 : 4;
}

STATIC VOID G2048Reset(VOID) {
    gBS->SetMem(g2048Board, sizeof(g2048Board), 0);
    g2048Score = 0; g2048GameOver = FALSE; g2048Won = FALSE;
    G2048Spawn(); G2048Spawn();
}

STATIC BOOLEAN G2048Slide(INT32 *line) {
    INT32 out[G2048_N] = {0}, oi = 0, last = -1;
    BOOLEAN moved = FALSE;
    for (INT32 i=0;i<G2048_N;i++) {
        if (!line[i]) continue;
        if (last >= 0 && out[last] == line[i]) {
            out[last] *= 2; g2048Score += out[last];
            if (out[last] == 2048) g2048Won = TRUE;
            last = -1; moved = TRUE;
        } else { out[oi /* oi ._. */] = line[i]; last = oi; oi++; moved = TRUE; }
    }
    for (INT32 i=0;i<G2048_N;i++) { if (line[i] != out[i]) moved = TRUE; line[i] = out[i]; }
    return moved;
}

STATIC BOOLEAN G2048Move(INT32 dx, INT32 dy) {
    if (g2048GameOver) return FALSE;
    BOOLEAN moved = FALSE;
    for (INT32 i=0;i<G2048_N;i++) {
        INT32 line[G2048_N];
        for (INT32 j=0;j<G2048_N;j++) {
            INT32 x = dx>0 ? G2048_N-1-j : dx<0 ? j : i;
            INT32 y = dy>0 ? G2048_N-1-j : dy<0 ? j : i;
            line[j] = (INT32)g2048Board[y][x];
        }
        if (G2048Slide(line)) moved = TRUE;
        for (INT32 j=0;j<G2048_N;j++) {
            INT32 x = dx>0 ? G2048_N-1-j : dx<0 ? j : i;
            INT32 y = dy>0 ? G2048_N-1-j : dy<0 ? j : i;
            g2048Board[y][x] = (UINT32)line[j];
        }
    }
    if (moved) {
        G2048Spawn();
        BOOLEAN any = FALSE;
        for (INT32 y=0;y<G2048_N;y++) for (INT32 x=0;x<G2048_N;x++) if (!g2048Board[y][x]) any = TRUE;
        if (!any) {
            BOOLEAN canMove = FALSE;
            for (INT32 y=0;y<G2048_N;y++) for (INT32 x=0;x<G2048_N;x++) {
                if (x+1<G2048_N && g2048Board[y][x]==g2048Board[y][x+1]) canMove = TRUE;
                if (y+1<G2048_N && g2048Board[y][x]==g2048Board[y+1][x]) canMove = TRUE;
            }
            if (!canMove) g2048GameOver = TRUE;
        }
    }
    return moved;
}

STATIC UINT32 G2048Color(UINT32 v) {
    switch (v) {
        case 0:    return RGB(30,41,59);
        case 2:    return RGB(238,228,218);
        case 4:    return RGB(237,224,200);
        case 8:    return RGB(242,177,121);
        case 16:   return RGB(245,149,99);
        case 32:   return RGB(246,124,95);
        case 64:   return RGB(246,94,59);
        case 128:  return RGB(237,207,114);
        case 256:  return RGB(237,204,97);
        case 512:  return RGB(237,200,80);
        case 1024: return RGB(237,197,63);
        case 2048: return RGB(237,194,46);
        default:   return RGB(60,60,60);
    }
}

VOID Render2048(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12, Y = Win->Y + 36;
    CHAR16 Buf[64];
    UnicodeSPrint(Buf, sizeof(Buf), L"Score: %d  Best: %d", g2048Score, g2048Best);
    DrawString(Buf, X, Y, COLOR_TEXT);
    INT32 BoardX = X, BoardY = Y + 22;
    DrawRect(BoardX-2, BoardY-2, G2048_N*G2048_CELL+4, G2048_N*G2048_CELL+4, COLOR_WIN_BORDER);
    for (INT32 y=0;y<G2048_N;y++) for (INT32 x=0;x<G2048_N;x++) {
        UINT32 v = g2048Board[y][x];
        INT32 px = BoardX + x*G2048_CELL, py = BoardY + y*G2048_CELL;
        DrawRect(px+2, py+2, G2048_CELL-4, G2048_CELL-4, G2048Color(v));
        if (v) {
            CHAR16 NB[8]; UnicodeSPrint(NB, sizeof(NB), L"%d", v);
            DrawString(NB, px + G2048_CELL/2 - TextW(NB)/2, py + G2048_CELL/2 - CharH()/2, RGB(30,30,30));
        }
    }
    DrawButton(X, BoardY + G2048_N*G2048_CELL + 10, 90, 26, L"New Game", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    if (g2048GameOver) DrawString(L"GAME OVER", X+120, BoardY+G2048_N*G2048_CELL+16, COLOR_BTN_CLOSE);
    else if (g2048Won)  DrawString(L"YOU WIN!",  X+120, BoardY+G2048_N*G2048_CELL+16, NEW_APP_COLOR_GOOD);
}

BOOLEAN Input2048(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 12, Y = Win->Y + 36;
    INT32 BoardY = Y + 22;
    if (IsInRect(MX,MY,X,BoardY+G2048_N*G2048_CELL+10,90,26)) G2048Reset();
    return TRUE;
}

STATIC VOID PongResetBall(INT32 dir) {
    gPongBallX = PONG_W/2; gPongBallY = PONG_H/2;
    gPongBallVX = 5*dir; gPongBallVY = (INT32)((gSnakeTick & 1) ? 4 : -4);
    gPongPlayerY = gPongAiY = (PONG_H - PONG_PH)/2;
}

VOID UpdatePong(VOID) {
    if (gPongGameOver) return;
    gPongBallX += gPongBallVX; gPongBallY += gPongBallVY;
    if (gPongBallY <= 0 || gPongBallY >= PONG_H - 6) gPongBallVY = -gPongBallVY;
    if (gPongBallX <= PONG_PW + 4 && gPongBallY >= gPongPlayerY && gPongBallY <= gPongPlayerY+PONG_PH)
        gPongBallVX = -gPongBallVX;
    if (gPongBallX >= PONG_W - PONG_PW - 10 && gPongBallY >= gPongAiY && gPongBallY <= gPongAiY+PONG_PH)
        gPongBallVX = -gPongBallVX;
    if (gPongBallX < 0)     { gPongScoreA++; PongResetBall(1);  }
    if (gPongBallX > PONG_W){ gPongScoreP++; PongResetBall(-1); }
    if (gPongScoreP >= 7 || gPongScoreA >= 7) gPongGameOver = TRUE;
    INT32 target = gPongBallY - PONG_PH/2;
    if (target > gPongAiY + 1) gPongAiY += 1;
    else if (target < gPongAiY - 1) gPongAiY -= 1;
    if (gPongAiY < 0) gPongAiY = 0;
    if (gPongAiY > PONG_H - PONG_PH) gPongAiY = PONG_H - PONG_PH;
}

VOID RenderPong(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12, Y = Win->Y + 36;
    CHAR16 Buf[64];
    UnicodeSPrint(Buf, sizeof(Buf), L"  YOU  %d     -     %d  CPU  ", gPongScoreP, gPongScoreA);
    DrawString(Buf, X + 100, Y, gPongGameOver ? COLOR_BTN_CLOSE : NEW_APP_COLOR_ACCENT);
    INT32 FX = X, FY = Y + 20;
    DrawRect(FX-2, FY-2, PONG_W+4, PONG_H+4, COLOR_WIN_BORDER);
    DrawRect(FX, FY, PONG_W, PONG_H, RGB(10,15,25));
    DrawRect(FX+PONG_W/2-1, FY, 2, PONG_H, RGB(60,70,90));
    DrawRect(FX+4, FY+gPongPlayerY, PONG_PW, PONG_PH, NEW_APP_COLOR_GOOD);
    DrawRect(FX+PONG_W-PONG_PW-4, FY+gPongAiY, PONG_PW, PONG_PH, COLOR_BTN_CLOSE);
    DrawRect(FX+gPongBallX, FY+gPongBallY, 6, 6, RGB(255,255,255));
    DrawButton(X, FY+PONG_H+8, 90, 24, L"New Game", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    if (gPongGameOver) DrawString(L"MATCH OVER", X+120, FY+PONG_H+14, COLOR_BTN_CLOSE);
}

BOOLEAN InputPong(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 12, FY = Win->Y + 36 + 20;
    if (IsInRect(MX,MY,X,FY+PONG_H+8,90,24)) {
        gPongScoreP = gPongScoreA = 0; gPongGameOver = FALSE; PongResetBall(1);
    }
    return TRUE;
}

VOID RenderHexViewer(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawButton(X, Y, 90, 24, L"Open File", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X+95, Y, 60, 24, L"Up",   COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+160, Y, 60, 24, L"Down", COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawString(gHexFileName, X+230, Y+8, COLOR_TEXT);
    INT32 DY = Y + 34;
    DrawRect(X, DY, Win->W-20, Win->H-80, COLOR_CANVAS);
    UINTN rows = gHexLen / HEX_BYTES_PER_ROW + (gHexLen % HEX_BYTES_PER_ROW ? 1 : 0);
    INT32 maxRows = (Win->H - 80) / (CharH()+2);
    for (INT32 r = 0; r < maxRows && (UINTN)(r + gHexScroll) < rows; r++) {
        UINTN off = (gHexScroll + r) * HEX_BYTES_PER_ROW;
        CHAR16 Line[128], Tmp[16];
        UnicodeSPrint(Line, sizeof(Line), L"%04x  ", (UINT32)off);
        for (INT32 i = 0; i < HEX_BYTES_PER_ROW; i++) {
            UINTN p = off + i;
            if (p < gHexLen) UnicodeSPrint(Tmp, sizeof(Tmp), L"%02x ", gHexData[p]);
            else             UnicodeSPrint(Tmp, sizeof(Tmp), L"   ");
            StrCatS(Line, 128, Tmp);
        }
        StrCatS(Line, 128, L" ");
        for (INT32 i = 0; i < HEX_BYTES_PER_ROW; i++) {
            UINTN p = off + i;
            CHAR16 ch[2] = { (p<gHexLen && gHexData[p]>=32 && gHexData[p]<127) ? gHexData[p] : L'.', 0 };
            StrCatS(Line, 128, ch);
        }
        DrawString(Line, X+6, DY+4 + r*(CharH()+2), COLOR_TEXT);
    }
}

BOOLEAN InputHexViewer(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    if (IsInRect(MX,MY,X,Y,90,24)) {
        FileDialogOpen(FILEDLG_OPEN, FD_P_HEX_LOAD);
        return TRUE;
    }
    if (IsInRect(MX,MY,X+95,Y,60,24) && gHexScroll>0) { gHexScroll--; return TRUE; }
    if (IsInRect(MX,MY,X+160,Y,60,24)) {
        UINTN rows = gHexLen / HEX_BYTES_PER_ROW + (gHexLen % HEX_BYTES_PER_ROW ? 1 : 0);
        if (gHexScroll + 1 < rows) gHexScroll++;
        return TRUE;
    }
    return TRUE;
}

STATIC VOID MSReset(VOID) {
    gBS->SetMem(gMSBoard, sizeof(gMSBoard), 0);
    gBS->SetMem(gMSState, sizeof(gMSState), 0);
    gMSGameOver = gMSWon = FALSE;
    gMSFirstClick = TRUE;
}

STATIC VOID MSPlaceMines(INT32 sx, INT32 sy) {
    INT32 placed = 0;
    while (placed < MS_MINES) {
        gMSRng = gMSRng*1664525u + 1013904223u;
        INT32 x = gMSRng % MS_W;
        gMSRng = gMSRng*1664525u + 1013904223u;
        INT32 y = gMSRng % MS_H;
        if (gMSBoard[y][x] == 9) continue;
        if (x == sx && y == sy) continue;
        gMSBoard[y][x] = 9; placed++;
    }
    for (INT32 y=0;y<MS_H;y++) for (INT32 x=0;x<MS_W;x++) {
        if (gMSBoard[y][x] == 9) continue;
        UINT8 n = 0;
        for (INT32 dy=-1;dy<=1;dy++) for (INT32 dx=-1;dx<=1;dx++) {
            INT32 nx=x+dx, ny=y+dy;
            if (nx>=0&&nx<MS_W&&ny>=0&&ny<MS_H && gMSBoard[ny][nx]==9) n++;
        }
        gMSBoard[y][x] = n;
    }
}

STATIC VOID MSReveal(INT32 x, INT32 y) {
    if (x<0||x>=MS_W||y<0||y>=MS_H) return;
    if (gMSState[y][x]) return;
    gMSState[y][x] = 1;
    if (gMSBoard[y][x] == 9) { gMSGameOver = TRUE; return; }
    if (gMSBoard[y][x] == 0)
        for (INT32 dy=-1;dy<=1;dy++) for (INT32 dx=-1;dx<=1;dx++)
            if (dx||dy) MSReveal(x+dx, y+dy);
}

STATIC BOOLEAN MSIsWon(VOID) {
    for (INT32 y=0;y<MS_H;y++) for (INT32 x=0;x<MS_W;x++)
        if (gMSBoard[y][x]!=9 && !gMSState[y][x]) return FALSE;
    return TRUE;
}

VOID RenderMinesweeper(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    INT32 BoardX = X, BoardY = Y + 34;
    for (INT32 y=0;y<MS_H;y++) for (INT32 x=0;x<MS_W;x++) {
        INT32 px = BoardX + x*MS_CELL, py = BoardY + y*MS_CELL;
        UINT32 bg;
        if (gMSState[y][x] == 1) bg = RGB(200,200,200);
        else if (gMSState[y][x] == 2) bg = RGB(200,150,40);
        else bg = RGB(80,90,120);
        DrawRect(px, py, MS_CELL-2, MS_CELL-2, bg);
        if (gMSState[y][x] == 1) {
            if (gMSBoard[y][x] == 9) {
                DrawRect(px+6, py+6, MS_CELL-14, MS_CELL-14, RGB(220,40,40));
            } else if (gMSBoard[y][x]) {
                CHAR16 NB[4]; UnicodeSPrint(NB, sizeof(NB), L"%d", gMSBoard[y][x]);
                DrawString(NB, px+8, py+7, RGB(20,20,20));
            }
        }
    }
    DrawButton(X, BoardY + MS_H*MS_CELL + 8, 90, 26, L"New Game", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    if (gMSGameOver) DrawString(L"BOOM!", X+110, BoardY+MS_H*MS_CELL+16, COLOR_BTN_CLOSE);
    else if (gMSWon)  DrawString(L"CLEARED!", X+110, BoardY+MS_H*MS_CELL+16, NEW_APP_COLOR_GOOD);
}

BOOLEAN InputMinesweeper(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    INT32 BoardX = X, BoardY = Y + 34;
    if (IsInRect(MX,MY,X,BoardY + MS_H*MS_CELL + 8,90,26)) { MSReset(); return TRUE; }
    for (INT32 y=0;y<MS_H;y++) for (INT32 x=0;x<MS_W;x++) {
        INT32 px = BoardX + x*MS_CELL, py = BoardY + y*MS_CELL;
        if (!IsInRect(MX,MY,px,py,MS_CELL-2,MS_CELL-2)) continue;
        if (gMSGameOver || gMSWon) return TRUE;
        if (gMSFirstClick) { MSPlaceMines(x,y); gMSFirstClick = FALSE; }
        MSReveal(x,y);
        if (!gMSGameOver && MSIsWon()) gMSWon = TRUE;
        return TRUE;
    }
    return TRUE;
}

STATIC UINT32 CPValueFromX(INT32 X, INT32 BarX, INT32 BarW) {
    INT32 v = (X - BarX) * 255 / BarW;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (UINT32)v;
}

VOID RenderColorPicker(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12, Y = Win->Y + 40;
    INT32 BarW = 260, BarH = 20;
    DrawString(L"R", X, Y, COLOR_TEXT);
    DrawRect(X+20, Y, BarW, BarH, RGB(60,60,60));
    DrawRect(X+20, Y, (INT32)(gCP_R * BarW / 255), BarH, RGB(220,60,60));
    DrawRect(X+20+(INT32)(gCP_R * BarW / 255)-2, Y-2, 4, BarH+4, RGB(255,255,255));
    DrawString(L"G", X, Y+30, COLOR_TEXT);
    DrawRect(X+20, Y+30, BarW, BarH, RGB(60,60,60));
    DrawRect(X+20, Y+30, (INT32)(gCP_G * BarW / 255), BarH, RGB(60,220,60));
    DrawRect(X+20+(INT32)(gCP_G * BarW / 255)-2, Y+28, 4, BarH+4, RGB(255,255,255));
    DrawString(L"B", X, Y+60, COLOR_TEXT);
    DrawRect(X+20, Y+60, BarW, BarH, RGB(60,60,60));
    DrawRect(X+20, Y+60, (INT32)(gCP_B * BarW / 255), BarH, RGB(60,60,220));
    DrawRect(X+20+(INT32)(gCP_B * BarW / 255)-2, Y+58, 4, BarH+4, RGB(255,255,255));
    UINT32 C = RGB(gCP_R, gCP_G, gCP_B);
    DrawRect(X-2, Y+98, 124, 94, RGB(200,200,200));
    DrawRect(X, Y+100, 120, 90, C);
    UnicodeSPrint(gCP_HexBuf, sizeof(gCP_HexBuf), L"#%02x%02x%02x", gCP_R, gCP_G, gCP_B);
    DrawString(gCP_HexBuf, X+140, Y+120, COLOR_TEXT);
    DrawButton(X+140, Y+150, 130, 26, L"Set Wallpaper A", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X+140, Y+180, 130, 26, L"Set Wallpaper B", COLOR_TOOLBAR_BG, COLOR_TEXT);
}

BOOLEAN InputColorPicker(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    INT32 X = Win->X + 12, Y = Win->Y + 40;
    INT32 BarW = 260, BarH = 20;
    if (Click) {
        if      (IsInRect(MX,MY,X+20,Y,BarW,BarH))       gCP_ActiveSlider = 0;
        else if (IsInRect(MX,MY,X+20,Y+30,BarW,BarH))    gCP_ActiveSlider = 1;
        else if (IsInRect(MX,MY,X+20,Y+60,BarW,BarH))    gCP_ActiveSlider = 2;
        else if (!Last) gCP_ActiveSlider = -1;
        if (gCP_ActiveSlider == 0) gCP_R = CPValueFromX(MX, X+20, BarW);
        if (gCP_ActiveSlider == 1) gCP_G = CPValueFromX(MX, X+20, BarW);
        if (gCP_ActiveSlider == 2) gCP_B = CPValueFromX(MX, X+20, BarW);
        if (!Last) {
            if (IsInRect(MX,MY,X+140,Y+150,130,26)) { gWallpaperColor1 = RGB(gCP_R,gCP_G,gCP_B); gWallpaperCacheDirty=TRUE; }
            if (IsInRect(MX,MY,X+140,Y+180,130,26)) { gWallpaperColor2 = RGB(gCP_R,gCP_G,gCP_B); gWallpaperCacheDirty=TRUE; }
        }
    } else {
        gCP_ActiveSlider = -1;
    }
    return TRUE;
}

/* ============================================================
 * HASH TOOL - MD5 / SHA-1
 * ============================================================ */
STATIC UINT32 HashRotl32(UINT32 x, UINT32 c) {
    return (x << c) | (x >> (32 - c));
}

STATIC VOID HexEncode(CONST UINT8 *bytes, UINTN len, CHAR16 *out) {
    STATIC CONST CHAR16 H[] = L"0123456789abcdef";
    for (UINTN i = 0; i < len; i++) {
        out[i*2]     = H[(bytes[i] >> 4) & 0xF];
        out[i*2 + 1] = H[bytes[i] & 0xF];
    }
    out[len*2] = L'\0';
}

STATIC VOID HashInputToBytes(UINT8 *dst, UINTN *dstLen) {
    *dstLen = gHashInputLen;
    for (UINTN i = 0; i < gHashInputLen; i++) dst[i] = (UINT8)(gHashInput[i] & 0xFF);
}

STATIC VOID MD5Compute(CONST UINT8 *data, UINTN len, UINT8 out[16]) {
    STATIC CONST UINT32 S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };
    STATIC CONST UINT32 K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };

    UINT32 a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;

    UINTN padded = len + 1;
    while (padded % 64 != 56) padded++;
    padded += 8;

    UINT8 *msg = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, padded, (VOID**)&msg))) return;
    gBS->SetMem(msg, padded, 0);
    if (len > 0) gBS->CopyMem(msg, (VOID *)data, len);
    msg[len] = 0x80;
    UINT64 bits = (UINT64)len * 8;
    for (INT32 i = 0; i < 8; i++) msg[padded - 8 + i] = (UINT8)(bits >> (8 * i));

    for (UINTN off = 0; off < padded; off += 64) {
        UINT32 M[16];
        for (INT32 i = 0; i < 16; i++)
            M[i] = (UINT32)msg[off + i*4] |
                   ((UINT32)msg[off + i*4 + 1] <<  8) |
                   ((UINT32)msg[off + i*4 + 2] << 16) |
                   ((UINT32)msg[off + i*4 + 3] << 24);

        UINT32 A = a0, B = b0, C = c0, D = d0;
        for (INT32 i = 0; i < 64; i++) {
            UINT32 F; INT32 g;
            if (i < 16)      { F = (B & C) | (~B & D);   g = i; }
            else if (i < 32) { F = (D & B) | (~D & C);   g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;            g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);         g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + HashRotl32(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }

    gBS->FreePool(msg);
    for (INT32 i = 0; i < 4; i++) out[i]      = (UINT8)(a0 >> (i*8));
    for (INT32 i = 0; i < 4; i++) out[4 + i]  = (UINT8)(b0 >> (i*8));
    for (INT32 i = 0; i < 4; i++) out[8 + i]  = (UINT8)(c0 >> (i*8));
    for (INT32 i = 0; i < 4; i++) out[12 + i] = (UINT8)(d0 >> (i*8));
}

STATIC VOID SHA1Compute(CONST UINT8 *data, UINTN len, UINT8 out[20]) {
    UINT32 h0 = 0x67452301;
    UINT32 h1 = 0xEFCDAB89;
    UINT32 h2 = 0x98BADCFE;
    UINT32 h3 = 0x10325476;
    UINT32 h4 = 0xC3D2E1F0;

    UINTN padded = len + 1;
    while (padded % 64 != 56) padded++;
    padded += 8;

    UINT8 *msg = NULL;
    if (EFI_ERROR(gBS->AllocatePool(EfiLoaderData, padded, (VOID**)&msg))) return;
    gBS->SetMem(msg, padded, 0);
    if (len > 0) gBS->CopyMem(msg, (VOID *)data, len);
    msg[len] = 0x80;
    UINT64 bits = (UINT64)len * 8;
    for (INT32 i = 0; i < 8; i++) msg[padded - 1 - i] = (UINT8)(bits >> (8 * i));

    for (UINTN off = 0; off < padded; off += 64) {
        UINT32 w[80];
        for (INT32 i = 0; i < 16; i++)
            w[i] = ((UINT32)msg[off + i*4]     << 24) |
                   ((UINT32)msg[off + i*4 + 1] << 16) |
                   ((UINT32)msg[off + i*4 + 2] <<  8) |
                   ((UINT32)msg[off + i*4 + 3]);
        for (INT32 i = 16; i < 80; i++)
            w[i] = HashRotl32(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        UINT32 a = h0, b = h1, c = h2, d = h3, e = h4;
        for (INT32 i = 0; i < 80; i++) {
            UINT32 f, k;
            if      (i < 20) { f = (b & c) | (~b & d);            k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
            UINT32 tmp = HashRotl32(a, 5) + f + e + k + w[i];
            e = d; d = c; c = HashRotl32(b, 30); b = a; a = tmp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    gBS->FreePool(msg);
    for (INT32 i = 0; i < 4; i++) out[i]      = (UINT8)(h0 >> (24 - i*8));
    for (INT32 i = 0; i < 4; i++) out[4 + i]  = (UINT8)(h1 >> (24 - i*8));
    for (INT32 i = 0; i < 4; i++) out[8 + i]  = (UINT8)(h2 >> (24 - i*8));
    for (INT32 i = 0; i < 4; i++) out[12 + i] = (UINT8)(h3 >> (24 - i*8));
    for (INT32 i = 0; i < 4; i++) out[16 + i] = (UINT8)(h4 >> (24 - i*8));
}

STATIC VOID HashComputeBoth(VOID) {
    UINT8 bytes[HASH_INPUT_MAX];
    UINTN n = 0;
    HashInputToBytes(bytes, &n);
    UINT8 md5[16], sha1[20];
    MD5Compute(bytes, n, md5);
    SHA1Compute(bytes, n, sha1);
    HexEncode(md5, 16, gHashMd5Out);
    HexEncode(sha1, 20, gHashSha1Out);
}

VOID RenderHash(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawString(L"Hash Tool - MD5 / SHA-1", X, Y, COLOR_TEXT);
    DrawString(L"Input:", X, Y+22, RGB(148,163,184));
    DrawRect(X, Y+40, Win->W-20, 60, COLOR_CANVAS);
    DrawString(gHashInput, X+4, Y+44, COLOR_TEXT);
    DrawRect(X+4 + (INT32)gHashInputLen * CharW(), Y+42, 1, CharH()+2, COLOR_CURSOR);
    DrawButton(X,          Y+110, 110, 24, L"Compute", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X+120,      Y+110, 90,  24, L"Clear",   COLOR_BTN_CLOSE,    COLOR_TEXT);
    DrawString(L"MD5:",  X,      Y+150, RGB(148,163,184));
    DrawString(gHashMd5Out[0]  ? gHashMd5Out  : L"(press Compute)", X+50, Y+150, gHashMd5Out[0]  ? NEW_APP_COLOR_GOOD : RGB(100,116,139));
    DrawString(L"SHA1:", X,      Y+172, RGB(148,163,184));
    DrawString(gHashSha1Out[0] ? gHashSha1Out : L"(press Compute)", X+50, Y+172, gHashSha1Out[0] ? NEW_APP_COLOR_GOOD : RGB(100,116,139));
    DrawString(L"Type text in input box. Enter = compute.", X, Win->Y+Win->H-24, RGB(100,116,139));
}

BOOLEAN InputHash(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    if (IsInRect(MX,MY,X,Y+110,110,24))  { HashComputeBoth(); return TRUE; }
    if (IsInRect(MX,MY,X+120,Y+110,90,24)) {
        gHashInput[0] = L'\0'; gHashInputLen = 0;
        gHashMd5Out[0] = L'\0'; gHashSha1Out[0] = L'\0';
        return TRUE;
    }
    return TRUE;
}


STATIC CONST CHAR16 gB64Alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

STATIC INT32 B64Value(CHAR16 c) {
    for (INT32 i = 0; i < 64; i++) if (gB64Alphabet[i] == c) return i;
    return -1;
}

STATIC VOID B64Encode(VOID) {
    gB64OutputLen = 0;
    for (UINTN i = 0; i < gB64InputLen; i += 3) {
        UINT8 b0 = (UINT8)(gB64Input[i] & 0xFF);
        UINT8 b1 = (i+1 < gB64InputLen) ? (UINT8)(gB64Input[i+1] & 0xFF) : 0;
        UINT8 b2 = (i+2 < gB64InputLen) ? (UINT8)(gB64Input[i+2] & 0xFF) : 0;
        if (gB64OutputLen + 4 > B64_BUF * 2 - 1) break;
        gB64Output[gB64OutputLen++] = gB64Alphabet[b0 >> 2];
        gB64Output[gB64OutputLen++] = gB64Alphabet[((b0 & 3) << 4) | (b1 >> 4)];
        gB64Output[gB64OutputLen++] = (i+1 < gB64InputLen) ? gB64Alphabet[((b1 & 15) << 2) | (b2 >> 6)] : L'=';
        gB64Output[gB64OutputLen++] = (i+2 < gB64InputLen) ? gB64Alphabet[b2 & 63] : L'=';
    }
    gB64Output[gB64OutputLen] = L'\0';
}

STATIC VOID B64Decode(VOID) {
    gB64OutputLen = 0;
    INT32 buf[4]; UINTN bi = 0;
    for (UINTN i = 0; i < gB64InputLen; i++) {
        if (gB64Input[i] == L'=') break;
        INT32 v = B64Value(gB64Input[i]);
        if (v < 0) continue;
        buf[bi++] = v;
        if (bi == 4) {
            if (gB64OutputLen + 3 > B64_BUF * 2 - 1) break;
            gB64Output[gB64OutputLen++] = (CHAR16)((buf[0] << 2) | (buf[1] >> 4));
            gB64Output[gB64OutputLen++] = (CHAR16)(((buf[1] & 15) << 4) | (buf[2] >> 2));
            gB64Output[gB64OutputLen++] = (CHAR16)(((buf[2] & 3) << 6) | buf[3]);
            bi = 0;
        }
    }
    gB64Output[gB64OutputLen] = L'\0';
}

VOID RenderBase64(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawString(L"Input:", X, Y, COLOR_TEXT);
    DrawRect(X, Y+18, Win->W-20, 60, COLOR_CANVAS);
    DrawString(gB64Input, X+4, Y+22, COLOR_TEXT);
    DrawButton(X,           Y+88, 90, 24, L"Encode",   COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X+96,        Y+88, 90, 24, L"Decode",   COLOR_TOOLBAR_BG,   COLOR_TEXT);
    DrawButton(X+192,       Y+88, 90, 24, L"Clear",    COLOR_BTN_CLOSE,    COLOR_TEXT);
    DrawString(L"Output:", X, Y+124, COLOR_TEXT);
    DrawRect(X, Y+142, Win->W-20, 100, COLOR_CANVAS);
    DrawString(gB64Output, X+4, Y+146, NEW_APP_COLOR_GOOD);
    DrawString(L"Type in input field. Enter = Encode.", X, Win->Y+Win->H-24, RGB(100,116,139));
}

BOOLEAN InputBase64(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    if (IsInRect(MX,MY,X,    Y+88,90,24)) { B64Encode(); return TRUE; }
    if (IsInRect(MX,MY,X+96, Y+88,90,24)) { B64Decode(); return TRUE; }
    if (IsInRect(MX,MY,X+192,Y+88,90,24)) {
        gB64Input[0]=L'\0'; gB64InputLen=0;
        gB64Output[0]=L'\0'; gB64OutputLen=0;
        return TRUE;
    }
    return TRUE;
}

STATIC UINT32 PwRand(VOID) { gPwRng = gPwRng*1664525u + 1013904223u; return gPwRng; }

STATIC VOID PwGenerate(VOID) {
    CONST CHAR16 *upper = L"ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    CONST CHAR16 *lower = L"abcdefghijklmnopqrstuvwxyz";
    CONST CHAR16 *digits = L"0123456789";
    CONST CHAR16 *syms = L"!@#$%^&*()-_=+[]{}";
    CHAR16 pool[160]; UINTN poolLen = 0;
    if (gPwUseUpper)  for (UINTN i=0;upper[i];i++)  pool[poolLen++]=upper[i];
    if (gPwUseLower)  for (UINTN i=0;lower[i];i++)  pool[poolLen++]=lower[i];
    if (gPwUseDigits) for (UINTN i=0;digits[i];i++) pool[poolLen++]=digits[i];
    if (gPwUseSymbols)for (UINTN i=0;syms[i];i++)   pool[poolLen++]=syms[i];
    if (poolLen == 0 || gPwLen == 0) { gPwOutput[0]=L'\0'; return; }
    if (gPwLen > 63) gPwLen = 63;
    for (UINTN i = 0; i < gPwLen; i++) gPwOutput[i] = pool[PwRand() % poolLen];
    gPwOutput[gPwLen] = L'\0';
}

VOID RenderPasswordGen(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12, Y = Win->Y + 40;
    CHAR16 Buf[64];
    UnicodeSPrint(Buf,sizeof(Buf),L"Length: %d", gPwLen);
    DrawString(Buf, X, Y, COLOR_TEXT);
    DrawButton(X+130, Y-4, 34, 22, L"-", COLOR_WIN_BORDER, COLOR_TEXT);
    DrawButton(X+170, Y-4, 34, 22, L"+", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X,      Y+34, 140, 24, L"Upper A-Z", gPwUseUpper?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+146,  Y+34, 140, 24, L"Lower a-z", gPwUseLower?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X,      Y+64, 140, 24, L"Digits 0-9",gPwUseDigits?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+146,  Y+64, 140, 24, L"Symbols",   gPwUseSymbols?NEW_APP_COLOR_GOOD:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X, Y+108, 130, 26, L"Generate", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X+140, Y+108, 130, 26, L"Clear", COLOR_BTN_CLOSE, COLOR_TEXT);
    DrawRect(X, Y+148, Win->W-24, 40, COLOR_CANVAS);
    DrawString(gPwOutput, X+6, Y+162, NEW_APP_COLOR_ACCENT);
}

BOOLEAN InputPasswordGen(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 12, Y = Win->Y + 40;
    if (IsInRect(MX,MY,X+130,Y-4,34,22)) { if (gPwLen > 4) gPwLen -= 2; return TRUE; }
    if (IsInRect(MX,MY,X+170,Y-4,34,22)) { if (gPwLen < 63) gPwLen += 2; return TRUE; }
    if (IsInRect(MX,MY,X,     Y+34,140,24)) { gPwUseUpper = !gPwUseUpper; return TRUE; }
    if (IsInRect(MX,MY,X+146, Y+34,140,24)) { gPwUseLower = !gPwUseLower; return TRUE; }
    if (IsInRect(MX,MY,X,     Y+64,140,24)) { gPwUseDigits= !gPwUseDigits; return TRUE; }
    if (IsInRect(MX,MY,X+146, Y+64,140,24)) { gPwUseSymbols=!gPwUseSymbols; return TRUE; }
    if (IsInRect(MX,MY,X,     Y+108,130,26)) { PwGenerate(); return TRUE; }
    if (IsInRect(MX,MY,X+140, Y+108,130,26)) { gPwOutput[0]=L'\0'; return TRUE; }
    return TRUE;
}

VOID PianoStartNote(UINT32 Freq) {
    if (Freq == 0) return;
    UINT32 Divisor = 1193182 / Freq;
    io_outb(0x43, 0xB6);
    io_outb(0x42, (UINT8)(Divisor & 0xFF));
    io_outb(0x42, (UINT8)((Divisor >> 8) & 0xFF));
    UINT8 State = io_inb(0x61);
    if ((State & 0x03) != 0x03) io_outb(0x61, State | 0x03);
}

VOID PianoStopNote(VOID) { io_outb(0x61, io_inb(0x61) & ~0x03); }

/* Async Scarlet Fire player — never blocks */

VOID ScarletFireStart(VOID) {
    gScarletIdx = 0;
    gScarletNoteEndTsc = 0;
    gScarletPlaying = TRUE;
}

VOID ScarletFireStop(VOID) {
    gScarletPlaying = FALSE;
    PianoStopNote();
}

VOID UpdateScarletFire(VOID) {
    if (!gScarletPlaying) return;
    UINTN count = sizeof(gScarletFire) / sizeof(gScarletFire[0]);
    UINT64 now = read_tsc();
    if (gScarletNoteEndTsc != 0 && now < gScarletNoteEndTsc) return;
    while (gScarletIdx < count) {
        MUSIC_NOTE n = gScarletFire[gScarletIdx++];
        if (n.Frequency == 0) continue;
        PianoStartNote(n.Frequency);
        gScarletNoteEndTsc = now + (UINT64)n.Duration * 1000ULL * gTscPerUs;
        return;
    }
    gScarletPlaying = FALSE;
    PianoStopNote();
}

VOID UpdatePianoTick(VOID) {
    if (gPianoToneFrames > 0) {
        if (gPianoToneFrames == 1) PianoStopNote();
        gPianoToneFrames--;
    }
}

STATIC CONST UINT32 gPianoBaseFreqs[12] = {
    262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494
};

STATIC VOID PianoPlayKey(INT32 idx) {
    if (idx < 0 || idx >= 12) return;
    UINT32 f = gPianoBaseFreqs[idx];
    if (gPianoOctave == 5) f *= 2;
    else if (gPianoOctave == 3) f /= 2;
    PianoStartNote(f);
    gPianoToneFreq = f;
    gPianoToneFrames = 25;
    gPianoLastKey = idx;
}

VOID RenderPiano(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 12, Y = Win->Y + 40;
    CHAR16 Buf[32];
    UnicodeSPrint(Buf,sizeof(Buf),L"Octave: %d", gPianoOctave);
    DrawString(Buf, X, Y, COLOR_TEXT);
    DrawButton(X+140, Y-4, 60, 22, L"Oct -", COLOR_WIN_BORDER, COLOR_TEXT);
    DrawButton(X+206, Y-4, 60, 22, L"Oct +", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    CONST CHAR16 *WhiteKeys[] = { L"A", L"S", L"D", L"F", L"G", L"H", L"J" };
    CONST INT32   WhiteIdx[]  = { 0, 2, 4, 5, 7, 9, 11 };
    INT32 KW = 80, KH = 160, Ky = Y + 40;
    for (INT32 i = 0; i < 7; i++) {
        UINT32 bg = (gPianoLastKey == WhiteIdx[i] && gPianoToneFrames > 0) ? RGB(37,99,235) : RGB(240,240,240);
        DrawButton(X + i*KW, Ky, KW-2, KH, WhiteKeys[i], bg, RGB(20,20,20));
    }
    CONST CHAR16 *BlackKeys[] = { L"W", L"E", L"T", L"Y", L"U" };
    CONST INT32   BlackIdx[]  = { 1, 3, 6, 8, 10 };
    CONST INT32   BlackOff[]  = { 1, 2, 4, 5, 6 };
    for (INT32 i = 0; i < 5; i++) {
        UINT32 bg = (gPianoLastKey == BlackIdx[i] && gPianoToneFrames > 0) ? RGB(37,99,235) : RGB(20,20,20);
        DrawButton(X + BlackOff[i]*KW - 26, Ky, 52, 100, BlackKeys[i], bg, COLOR_TEXT);
    }
    DrawString(L"Keyboard: A S D F G H J = white, W E T Y U = black",
        X, Ky + KH + 12, RGB(148,163,184));
    DrawString(L"Each note plays for ~0.4 seconds.",
        X, Ky + KH + 28, RGB(100,116,139));
}

BOOLEAN InputPiano(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 12, Y = Win->Y + 40;
    INT32 KW = 80, KH = 160, Ky = Y + 40;
    if (IsInRect(MX,MY,X+140,Y-4,60,22)) { if (gPianoOctave > 1) gPianoOctave--; return TRUE; }
    if (IsInRect(MX,MY,X+206,Y-4,60,22)) { if (gPianoOctave < 8) gPianoOctave++; return TRUE; }
    CONST INT32 WhiteIdx[] = { 0, 2, 4, 5, 7, 9, 11 };
    for (INT32 i = 0; i < 7; i++)
        if (IsInRect(MX,MY,X+i*KW,Ky,KW-2,KH)) { PianoPlayKey(WhiteIdx[i]); return TRUE; }
    CONST INT32 BlackIdx[] = { 1, 3, 6, 8, 10 };
    CONST INT32 BlackOff[] = { 1, 2, 4, 5, 6 };
    for (INT32 i = 0; i < 5; i++)
        if (IsInRect(MX,MY,X+BlackOff[i]*KW-26,Ky,52,100)) { PianoPlayKey(BlackIdx[i]); return TRUE; }
    return TRUE;
}

STATIC UINT32 SolRand(VOID) { gSolRng = gSolRng*1664525u + 1013904223u; return gSolRng; }

STATIC BOOLEAN SolIsRed(SOL_CARD c) { return (c.Suit == 1 || c.Suit == 2); }

STATIC BOOLEAN SolCanFound(SOL_CARD c, INT32 f) {
    if (gSolFoundCount[f] == 0) return (c.Rank == 1);
    SOL_CARD top = gSolFound[f][gSolFoundCount[f]-1];
    if (top.Suit != c.Suit) return FALSE;
    return (c.Rank == top.Rank + 1);
}

STATIC BOOLEAN SolCanTab(SOL_CARD c, INT32 t) {
    if (gSolTabCount[t] == 0) return (c.Rank == 13);
    SOL_CARD top = gSolTab[t][gSolTabCount[t]-1];
    if (SolIsRed(top) == SolIsRed(c)) return FALSE;
    return (c.Rank == top.Rank - 1);
}

VOID SolNewGame(VOID) {
    SOL_CARD deck[52]; UINTN n = 0;
    for (UINT8 s = 0; s < 4; s++)
        for (UINT8 r = 1; r <= 13; r++) {
            deck[n].Rank = r; deck[n].Suit = s;
            deck[n].FaceUp = FALSE; deck[n].Exists = TRUE; n++;
        }
    for (UINTN i = 51; i > 0; i--) {
        UINTN j = SolRand() % (i+1);
        SOL_CARD t = deck[i]; deck[i] = deck[j]; deck[j] = t;
    }
    gBS->SetMem(gSolTab, sizeof(gSolTab), 0);
    for (INT32 t = 0; t < 7; t++) gSolTabCount[t] = 0;
    gBS->SetMem(gSolFound, sizeof(gSolFound), 0);
    for (INT32 f = 0; f < 4; f++) gSolFoundCount[f] = 0;
    gBS->SetMem(gSolStock, sizeof(gSolStock), 0);
    gBS->SetMem(gSolWaste, sizeof(gSolWaste), 0);
    gSolStockCount = 0; gSolWasteCount = 0;
    gSolSelPile = -1; gSolSelIdx = -1;
    gSolWon = FALSE;
    UINTN di = 0;
    for (INT32 t = 0; t < 7; t++)
        for (INT32 r = 0; r <= t; r++) {
            gSolTab[t][gSolTabCount[t]++] = deck[di++];
            if (r == t) gSolTab[t][gSolTabCount[t]-1].FaceUp = TRUE;
        }
    while (di < 52) gSolStock[gSolStockCount++] = deck[di++];
}

STATIC VOID SolDrawCard(INT32 x, INT32 y, SOL_CARD c, INT32 w, INT32 h) {
    if (!c.Exists) return;
    if (!c.FaceUp) {
        DrawRect(x, y, w, h, RGB(30,60,140));
        DrawRect(x+3, y+3, w-6, h-6, RGB(20,40,110));
        return;
    }
    DrawRect(x, y, w, h, RGB(240,240,240));
    DrawRect(x, y, w, h, RGB(220,220,220));
    DrawRect(x+2, y+2, w-4, h-4, RGB(255,255,255));
    CONST CHAR16 *Ranks[] = { L"", L"A", L"2", L"3", L"4", L"5", L"6", L"7", L"8", L"9", L"10", L"J", L"Q", L"K" };
    CONST CHAR16 *Suits[] = { L"S", L"H", L"D", L"C" };
    UINT32 col = SolIsRed(c) ? RGB(200,20,20) : RGB(20,20,20);
    DrawString(Ranks[c.Rank], x+6, y+6, col);
    DrawString(Suits[c.Suit], x+w-16, y+h-16, col);
}

VOID RenderSolitaire(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawButton(X, Y, 80, 22, L"New Game", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    INT32 CardW = 80, CardH = 100;
    INT32 TopY = Y + 32;
    INT32 SX = X, SY = TopY;
    if (gSolStockCount > 0) DrawRect(SX, SY, CardW, CardH, RGB(30,60,140));
    else DrawRect(SX, SY, CardW, CardH, COLOR_WIN_BORDER);
    DrawString(L"Stock", SX+18, SY+42, COLOR_TEXT);
    INT32 WX = SX + CardW + 12;
    if (gSolWasteCount > 0) SolDrawCard(WX, SY, gSolWaste[gSolWasteCount-1], CardW, CardH);
    else DrawRect(WX, SY, CardW, CardH, COLOR_WIN_BORDER);
    INT32 FX0 = X + 320;
    for (INT32 f = 0; f < 4; f++) {
        INT32 fx = FX0 + f * (CardW + 12);
        if (gSolFoundCount[f] > 0) SolDrawCard(fx, SY, gSolFound[f][gSolFoundCount[f]-1], CardW, CardH);
        else DrawRect(fx, SY, CardW, CardH, COLOR_WIN_BORDER);
    }
    INT32 TabY = SY + CardH + 30;
    for (INT32 t = 0; t < 7; t++) {
        INT32 tx = X + t * (CardW + 12);
        if (gSolTabCount[t] == 0) { DrawRect(tx, TabY, CardW, CardH, COLOR_WIN_BORDER); continue; }
        for (INT32 c = 0; c < gSolTabCount[t]; c++) {
            INT32 cy = TabY + c * 20;
            SolDrawCard(tx, cy, gSolTab[t][c], CardW, CardH);
        }
        INT32 lastY = TabY + (gSolTabCount[t]-1) * 20;
        if (gSolSelPile == t+1 && gSolSelIdx < gSolTabCount[t])
            DrawRect(tx-2, lastY-2, CardW+4, CardH+4, RGB(255,240,0));
    }
    if (gSolWon) DrawString(L"YOU WIN!", X+340, TopY+120, NEW_APP_COLOR_GOOD);
    DrawString(L"Click stock to draw. Click card to select, then click a pile.",
        X, Win->Y+Win->H-22, RGB(148,163,184));
}

BOOLEAN InputSolitaire(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    if (IsInRect(MX,MY,X,Y,80,22)) { SolNewGame(); return TRUE; }
    INT32 CardW = 80, CardH = 100;
    INT32 TopY = Y + 32;
    INT32 SX = X, SY = TopY, WX = SX + CardW + 12, FX0 = X + 320;
    INT32 TabY = SY + CardH + 30;
    if (IsInRect(MX,MY,SX,SY,CardW,CardH)) {
        if (gSolStockCount > 0) {
            gSolWaste[gSolWasteCount++] = gSolStock[--gSolStockCount];
            gSolWaste[gSolWasteCount-1].FaceUp = TRUE;
        } else {
            while (gSolWasteCount > 0) {
                gSolStock[gSolStockCount++] = gSolWaste[--gSolWasteCount];
                gSolStock[gSolStockCount-1].FaceUp = FALSE;
            }
        }
        gSolSelPile = -1; gSolSelIdx = -1;
        return TRUE;
    }
    if (gSolWasteCount > 0 && IsInRect(MX,MY,WX,SY,CardW,CardH)) {
        gSolSelPile = 0; gSolSelIdx = gSolWasteCount-1;
        return TRUE;
    }
    for (INT32 f = 0; f < 4; f++) {
        INT32 fx = FX0 + f*(CardW+12);
        if (IsInRect(MX,MY,fx,SY,CardW,CardH)) {
            if (gSolSelPile == 0) {
                SOL_CARD c = gSolWaste[gSolSelIdx];
                if (SolCanFound(c, f)) {
                    gSolFound[f][gSolFoundCount[f]++] = c;
                    gSolWasteCount--;
                    gSolSelPile = -1;
                    INT32 total = 0; for (INT32 k = 0; k < 4; k++) total += gSolFoundCount[k];
                    if (total == 52) gSolWon = TRUE;
                }
            } else if (gSolSelPile >= 1 && gSolSelIdx == gSolTabCount[gSolSelPile-1]-1) {
                INT32 t = gSolSelPile-1;
                SOL_CARD c = gSolTab[t][gSolSelIdx];
                if (SolCanFound(c, f)) {
                    gSolFound[f][gSolFoundCount[f]++] = c;
                    gSolTabCount[t]--;
                    if (gSolTabCount[t] > 0 && !gSolTab[t][gSolTabCount[t]-1].FaceUp)
                        gSolTab[t][gSolTabCount[t]-1].FaceUp = TRUE;
                    gSolSelPile = -1;
                }
            }
            return TRUE;
        }
    }
    for (INT32 t = 0; t < 7; t++) {
        INT32 tx = X + t*(CardW+12);
        INT32 count = gSolTabCount[t];
        INT32 topY = TabY + (count > 0 ? (count-1)*20 : 0);
        if (IsInRect(MX,MY,tx,topY,CardW,CardH)) {
            if (gSolSelPile >= 0) {
                SOL_CARD moving;
                if (gSolSelPile == 0) moving = gSolWaste[gSolSelIdx];
                else                  moving = gSolTab[gSolSelPile-1][gSolSelIdx];
                if (SolCanTab(moving, t)) {
                    if (gSolSelPile == 0) {
                        gSolTab[t][gSolTabCount[t]++] = moving;
                        gSolWasteCount--;
                    } else {
                        INT32 src = gSolSelPile - 1;
                        INT32 movingCount = gSolTabCount[src] - gSolSelIdx;
                        for (INT32 k = 0; k < movingCount; k++)
                            gSolTab[t][gSolTabCount[t]++] = gSolTab[src][gSolSelIdx + k];
                        gSolTabCount[src] -= movingCount;
                        if (gSolTabCount[src] > 0 && !gSolTab[src][gSolTabCount[src]-1].FaceUp)
                            gSolTab[src][gSolTabCount[src]-1].FaceUp = TRUE;
                    }
                    gSolSelPile = -1;
                }
                return TRUE;
            }
            for (INT32 c = count-1; c >= 0; c--) {
                INT32 cy = TabY + c*20;
                if (IsInRect(MX,MY,tx,cy,CardW,20)) {
                    if (gSolTab[t][c].FaceUp) { gSolSelPile = t+1; gSolSelIdx = c; }
                    return TRUE;
                }
            }
            return TRUE;
        }
    }
    return TRUE;
}

/* ============================================================
 * SOFTWARE STORE
 * ============================================================ */
VOID RenderStore(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawString(L"Software Store", X, Y, COLOR_TEXT);
    DrawString(L"Install / uninstall UEFIText apps.", X, Y+18, RGB(148,163,184));
    for (INT32 i = 0; i < gStoreAppCount; i++) {
        INT32 RowY = Y + 46 + i * 34;
        UINT32 rowBg = (gStoreSelected == i) ? COLOR_TITLE_ACTIVE : COLOR_TOOLBAR_BG;
        DrawRect(X, RowY, Win->W-20, 30, rowBg);
        DrawString(gStoreNames[i], X + 8, RowY + 10, COLOR_TEXT);
        CONST CHAR16 *status = gStoreInstalled[i] ? L"Installed" : L"Available";
        DrawString(status, X + 340, RowY + 10, gStoreInstalled[i] ? NEW_APP_COLOR_GOOD : NEW_APP_COLOR_WARN);
    }
    DrawButton(X, Win->Y+Win->H-40, 100, 26, L"Install",   NEW_APP_COLOR_GOOD, COLOR_TEXT);
    DrawButton(X+110, Win->Y+Win->H-40, 100, 26, L"Uninstall", COLOR_BTN_CLOSE, COLOR_TEXT);
    DrawString(L"Click a row, then Install/Uninstall.", X+220, Win->Y+Win->H-32, RGB(148,163,184));
}

BOOLEAN InputStore(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    for (INT32 i = 0; i < (INT32)gStoreAppCount; i++) {
        INT32 RowY = Y + 46 + i * 34;
        if (IsInRect(MX, MY, X, RowY, Win->W-20, 30)) { gStoreSelected = i; return TRUE; }
    }
    if (gStoreSelected >= 0) {
        UEFI_WINDOW *Target = gStoreTargets[gStoreSelected];
        if (IsInRect(MX, MY, X, Win->Y+Win->H-40, 100, 26)) {
            gStoreInstalled[gStoreSelected] = TRUE;
            for (UINTN k = 0; k < APP_COUNT; k++) {
                if (gAppRegistry[k].Win == Target) { gAppRegistry[k].InMenu = TRUE; break; }
            }
            Target->IsVisible = TRUE;
            Target->IsMinimized = FALSE;
            BringToFront(Target);
            return TRUE;
        }
        if (IsInRect(MX, MY, X+110, Win->Y+Win->H-40, 100, 26)) {
            gStoreInstalled[gStoreSelected] = FALSE;
            for (UINTN k = 0; k < APP_COUNT; k++) {
                if (gAppRegistry[k].Win == Target) { gAppRegistry[k].InMenu = FALSE; break; }
            }
            Target->IsVisible = FALSE;
            return TRUE;
        }
    }
    return TRUE;
}

/* ============================================================
 * BROWSER
 * ============================================================ */
STATIC CONST CHAR16 *gBrowserHomePage[] = {
    L"UEFIText Browser v1.0",
    L"",
    L"Type a URL above and press Enter.",
    L"Try: about.uefitext",
    L"Try: idkhow",
    L"Try: local",
    L"Try: google.com (not full copy just worked for uefitext ._.)"
};

STATIC CONST CHAR16 *gBrowserHashingPage[] = {
    L"hashing.com",
    L"",
    L"Welcome to hashing.com",
    L"The UEFIText hash utility hub.",
    L"",
    L"MD5 and SHA-1, computed from typed text.",
    L"Nothing leaves the machine. Ever.",
    L"",
    L"Press the button below to open the tool."
};

STATIC CONST CHAR16 *gBrowserAboutPage[] = {
    L"about.uefitext",
    L"",
    L"A tiny browser made of pixels and hope.",
    L"No HTML parser. No JS engine. No CSS.",
    L"Just CHAR16 strings and vibes.",
    L"",
    L"(c) 2026 UEFIText"
};

STATIC CONST CHAR16 *gBrowserLocalPage[] = {
    L"local://files",
    L"",
    L"Listing files on the EFI root...",
    L"(Use the File Manager for a real view.)",
    L"",
    L"Make files with: filemake <name>",
    L"Then use 'ls' in the shell to see them."
};

STATIC CONST CHAR16 *gBrowserIdkhowPage[] = {
    L"idkhow",
    L"",
    L"...",
    L"",
    L"Wait. You typed the magic word.",
    L"",
    L"You found a hidden page.",
    L"Press the button below to unlock the room."
};

BOOLEAN InputBrowser(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    if (IsInRect(MX, MY, X+40, Y, 120, 24)) {
        /* Back button - return to home */
        gBrowserPage = 0;
        return TRUE;
    }
    if (gBrowserPage == 3 && IsInRect(MX, MY, X, Win->Y+Win->H-40, 140, 26)) {
        gEasterEggUnlocked = TRUE;
        gEasterEggWin.IsVisible = TRUE;
        gEasterEggWin.IsMinimized = FALSE;
        BringToFront(&gEasterEggWin);
        return TRUE;
    }
    if (gBrowserPage == 4 && IsInRect(MX, MY, X, Win->Y+Win->H-40, 160, 26)) {
        gHashWin.IsVisible = TRUE;
        gHashWin.IsMinimized = FALSE;
        BringToFront(&gHashWin);
        return TRUE;
    }
    return TRUE;
}

VOID RenderBrowser(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawButton(X, Y, 34, 24, L"<", COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawRect(X+40, Y, Win->W-60, 24, COLOR_CANVAS);
    DrawString(gBrowserUrl, X+48, Y+8, COLOR_TEXT);
    DrawRect(X+46 + (INT32)gBrowserUrlLen * CharW(), Y+4, 1, 18, COLOR_CURSOR);
    INT32 BodyY = Y + 40;
    DrawRect(X, BodyY, Win->W-20, Win->H-96, RGB(245,245,245));
    CONST CHAR16 **page = gBrowserHomePage;
    INT32 pageLen = (INT32)(sizeof(gBrowserHomePage)/sizeof(gBrowserHomePage[0]));
    if (gBrowserPage == 1) { page = gBrowserAboutPage; pageLen = (INT32)(sizeof(gBrowserAboutPage)/sizeof(gBrowserAboutPage[0])); }
    else if (gBrowserPage == 2) { page = gBrowserLocalPage; pageLen = (INT32)(sizeof(gBrowserLocalPage)/sizeof(gBrowserLocalPage[0])); }
    else if (gBrowserPage == 3) { page = gBrowserIdkhowPage; pageLen = (INT32)(sizeof(gBrowserIdkhowPage)/sizeof(gBrowserIdkhowPage[0])); }
    else if (gBrowserPage == 4) { page = gBrowserHashingPage; pageLen = (INT32)(sizeof(gBrowserHashingPage)/sizeof(gBrowserHashingPage[0])); }
    for (INT32 i = 0; i < pageLen; i++) {
        UINT32 col = (i == 0) ? RGB(20,20,20) : RGB(60,60,60);
        DrawString(page[i], X+8, BodyY+8 + i*16, col);
    }
    if (gBrowserPage == 3) {
        DrawButton(X, Win->Y+Win->H-40, 140, 26, L"Open secret room", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    }
    if (gBrowserPage == 4) {
        DrawButton(X, Win->Y+Win->H-40, 160, 26, L"Open Hash Tool", COLOR_TITLE_ACTIVE, COLOR_TEXT);
    }
    DrawString(L"Type a URL then press Enter.", X+160, Win->Y+Win->H-32, RGB(100,116,139));
}

/* ============================================================
 * OFFICE
 * ============================================================ */
VOID RenderOffice(UEFI_WINDOW *Win) {
    DrawWindowTitleBar(Win);
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    DrawButton(X,     Y, 100, 24, L"Docs",   gOfficeTab==0?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+110, Y, 100, 24, L"Sheets", gOfficeTab==1?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+220, Y, 100, 24, L"Slides", gOfficeTab==2?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG, COLOR_TEXT);
    DrawButton(X+360, Y,  90, 24, L"Save",   COLOR_TITLE_ACTIVE, COLOR_TEXT);
    DrawButton(X+460, Y,  90, 24, L"Load",   COLOR_TOOLBAR_BG,   COLOR_TEXT);
    INT32 EditorY = Y + 34;
    INT32 EditorW = Win->W - 20;
    INT32 EditorH = Win->H - 120;

    if (gOfficeTab == 0) {
        DrawRect(X, EditorY, EditorW, EditorH, RGB(253,246,227));
        INT32 CurX = X + 8, CurY = EditorY + 8;
        for (UINTN i = 0; i < gOfficeDocLen; i++) {
            if (gOfficeDocBuf[i] == L'\n') { CurX = X + 8; CurY += CharH()+4; continue; }
            if (CurY + CharH() > EditorY + EditorH - 6) break;
            DrawChar(gOfficeDocBuf[i], CurX, CurY, RGB(60,60,60));
            CurX += CharW();
            if (CurX + CharW() >= X + EditorW - 8) { CurX = X + 8; CurY += CharH()+4; }
        }
        DrawRect(CurX, CurY, 1, CharH()+1, RGB(60,60,60));
        DrawString(L"Docs - plain text", X, Win->Y+Win->H-30, RGB(148,163,184));
    } else if (gOfficeTab == 1) {
        DrawRect(X, EditorY, EditorW, EditorH, RGB(20,20,20));
        INT32 CellW = (EditorW - 40) / 6;
        INT32 CellH = 22;
        INT32 GridX = X + 40;
        INT32 GridY = EditorY + 24;
        for (INT32 c = 0; c < 6; c++) {
            DrawRect(GridX + c*CellW, EditorY, CellW, 22, RGB(60,60,60));
            CHAR16 col[2] = { (CHAR16)(L'A' + c), 0 };
            DrawString(col, GridX + c*CellW + CellW/2 - 4, EditorY + 7, RGB(240,240,240));
        }
        for (INT32 r = 0; r < 8; r++) {
            DrawRect(X, GridY + r*CellH, 40, CellH, RGB(60,60,60));
            CHAR16 rn[4]; UnicodeSPrint(rn, sizeof(rn), L"%d", r+1);
            DrawString(rn, X + 6, GridY + r*CellH + 7, RGB(240,240,240));
            for (INT32 c = 0; c < 6; c++) {
                UINT32 bg = ((r+c) & 1) ? RGB(40,40,40) : RGB(30,30,30);
                DrawRect(GridX + c*CellW, GridY + r*CellH, CellW-1, CellH-1, bg);
            }
        }
        INT32 CurX = GridX + 4, CurY = GridY + 4;
        INT32 line = 0;
        for (UINTN i = 0; i < gOfficeSheetLen; i++) {
            if (gOfficeSheetBuf[i] == L'\n') { line++; CurX = GridX + 4; CurY = GridY + 4 + line*CellH; continue; }
            if (line >= 8) break;
            DrawChar(gOfficeSheetBuf[i], CurX, CurY, RGB(240,240,240));
            CurX += CharW();
            if (CurX + CharW() >= X + EditorW - 8) { line++; CurX = GridX + 4; CurY = GridY + 4 + line*CellH; }
        }
        DrawString(L"Sheets - A1 style grid", X, Win->Y+Win->H-30, RGB(148,163,184));
    } else {
        INT32 ListW = 140;
        DrawRect(X, EditorY, ListW, EditorH, RGB(30,30,30));
        DrawRect(X + ListW + 6, EditorY, EditorW - ListW - 6, EditorH, RGB(245,245,245));
        DrawString(L"Slides", X + 8, EditorY + 6, RGB(240,240,240));
        for (INT32 i = 0; i < 4; i++) {
            INT32 sy = EditorY + 26 + i*40;
            DrawRect(X + 6, sy, ListW - 12, 32, (i==0) ? RGB(80,80,120) : RGB(50,50,50));
            CHAR16 sn[8]; UnicodeSPrint(sn, sizeof(sn), L"Slide %d", i+1);
            DrawString(sn, X + 12, sy + 10, RGB(240,240,240));
        }
        INT32 slideX = X + ListW + 6;
        DrawString(L"UEFIText Presentation", slideX + 40, EditorY + 60, RGB(30,30,30));
        DrawString(L"Type to add body text below.", slideX + 20, EditorY + 90, RGB(100,100,100));
        INT32 CurX = slideX + 20, CurY = EditorY + 120;
        for (UINTN i = 0; i < gOfficeSlideLen; i++) {
            if (gOfficeSlideBuf[i] == L'\n') { CurX = slideX + 20; CurY += CharH()+4; continue; }
            if (CurY + CharH() > EditorY + EditorH - 6) break;
            DrawChar(gOfficeSlideBuf[i], CurX, CurY, RGB(60,60,60));
            CurX += CharW();
        }
        DrawRect(CurX, CurY, 1, CharH()+1, RGB(60,60,60));
        DrawString(L"Slides - presentation view", X, Win->Y+Win->H-30, RGB(148,163,184));
    }
}

BOOLEAN InputOffice(UEFI_WINDOW *Win, INT32 MX, INT32 MY, BOOLEAN Click, BOOLEAN Last) {
    if (!Click || Last) return TRUE;
    INT32 X = Win->X + 10, Y = Win->Y + 36;
    if (IsInRect(MX,MY,X,     Y, 100, 24)) { gOfficeTab = 0; return TRUE; }
    if (IsInRect(MX,MY,X+110, Y, 100, 24)) { gOfficeTab = 1; return TRUE; }
    if (IsInRect(MX,MY,X+220, Y, 100, 24)) { gOfficeTab = 2; return TRUE; }
    if (IsInRect(MX,MY,X+360, Y,  90, 24)) {
        if (gOfficeTab == 0) FileDialogOpen(FILEDLG_SAVE, FD_P_OFFICE_DOC_SAVE);
        if (gOfficeTab == 1) FileDialogOpen(FILEDLG_SAVE, FD_P_OFFICE_SHEET_SAVE);
        if (gOfficeTab == 2) FileDialogOpen(FILEDLG_SAVE, FD_P_OFFICE_SLIDE_SAVE);
        return TRUE;
    }
    if (IsInRect(MX,MY,X+460, Y,  90, 24)) {
        if (gOfficeTab == 0) FileDialogOpen(FILEDLG_OPEN, FD_P_OFFICE_DOC_LOAD);
        if (gOfficeTab == 1) FileDialogOpen(FILEDLG_OPEN, FD_P_OFFICE_SHEET_LOAD);
        if (gOfficeTab == 2) FileDialogOpen(FILEDLG_OPEN, FD_P_OFFICE_SLIDE_LOAD);
        return TRUE;
    }
    return TRUE;
}

/* ============================================================
 * INPUT / TASKBAR / MAIN LOOP
 * ============================================================ */
VOID PollMouse(BOOLEAN *OutLeftClick) {
    BOOLEAN Clicked=FALSE;
    STATIC UINT64 LastAbsX=0,LastAbsY=0;
    INT32 SpeedMul = (gSettingMouseSpeed == 0) ? 4 : (gSettingMouseSpeed == 2 ? 16 : 8);
    for (UINTN i=0;i<gNumMouseDevices;i++) {
        EFI_SIMPLE_POINTER_STATE State;
        if (!EFI_ERROR(gMouseDevices[i]->GetState(gMouseDevices[i],&State))) {
            INT32 dx=(INT32)State.RelativeMovementX;
            INT32 dy=(INT32)State.RelativeMovementY;
            if (gMouseDevices[i]->Mode&&gMouseDevices[i]->Mode->ResolutionX>0)
                dx=(INT32)((State.RelativeMovementX*SpeedMul)/(INT64)gMouseDevices[i]->Mode->ResolutionX);
            if (gMouseDevices[i]->Mode&&gMouseDevices[i]->Mode->ResolutionY>0)
                dy=(INT32)((State.RelativeMovementY*SpeedMul)/(INT64)gMouseDevices[i]->Mode->ResolutionY);
            gMouseX+=dx; gMouseY+=dy;
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
                    LastAbsX=AbsState.CurrentX; LastAbsY=AbsState.CurrentY;
                }
            }
            if (AbsState.ActiveButtons&(EFI_ABSPTR_TOUCH_OR_FIRST_BUTTON|EFI_ABSP_TouchActive)) Clicked=TRUE;
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
    gRedrawWindows = TRUE;

    if (!gEasterEggUnlocked) {
        BOOLEAN TextAppBusy =
            (gNotesWin.IsVisible && IsTopWindow(&gNotesWin)) ||
            (gMiniIDEWin.IsVisible && IsTopWindow(&gMiniIDEWin)) ||
            (gEditorWin.IsVisible && IsTopWindow(&gEditorWin)) ||
            (gTextTermWin.IsVisible && IsTopWindow(&gTextTermWin)) ||
            (gB64Win.IsVisible && IsTopWindow(&gB64Win)) ||
            (gOfficeWin.IsVisible && IsTopWindow(&gOfficeWin)) ||
            (gBrowserWin.IsVisible && IsTopWindow(&gBrowserWin));
        if (!TextAppBusy && Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E) {
            CHAR16 Ch = Key.UnicodeChar;
            if (Ch == gEasterSequence[gEasterSequencePos]) {
                gEasterSequencePos++;
                if (gEasterSequence[gEasterSequencePos] == L'\0') {
                    gEasterEggUnlocked = TRUE;
                    gEasterEggWin.IsVisible = TRUE; gEasterEggWin.IsMinimized = FALSE;
                    BringToFront(&gEasterEggWin);
                    gEasterSequencePos = 0;
                }
            } else {
                gEasterSequencePos = 0;
                if (Ch == gEasterSequence[0]) gEasterSequencePos = 1;
            }
        }
    }

    if (gBrowserWin.IsVisible && !gBrowserWin.IsMinimized && IsTopWindow(&gBrowserWin)) {
        if (Key.UnicodeChar==0x0D || Key.UnicodeChar==0x0A) {
            if (StrCmp(gBrowserUrl, L"idkhow") == 0) gBrowserPage = 3;
            else if (StrCmp(gBrowserUrl, L"about.uefitext") == 0) gBrowserPage = 1;
            else if (StrCmp(gBrowserUrl, L"local") == 0) gBrowserPage = 2;
            else if (StrCmp(gBrowserUrl, L"hashing.com") == 0) gBrowserPage = 4;
            else gBrowserPage = 0;
            gBrowserUrl[0] = L'\0'; gBrowserUrlLen = 0;
        } else if (Key.UnicodeChar==0x08) {
            if (gBrowserUrlLen > 0) { gBrowserUrlLen--; gBrowserUrl[gBrowserUrlLen] = L'\0'; }
        } else if (Key.UnicodeChar>=0x20 && Key.UnicodeChar<=0x7E && gBrowserUrlLen < BROWSER_URL_MAX-1) {
            gBrowserUrl[gBrowserUrlLen++] = Key.UnicodeChar;
            gBrowserUrl[gBrowserUrlLen] = L'\0';
        }
        return;
    }

    if (gOfficeWin.IsVisible && !gOfficeWin.IsMinimized && IsTopWindow(&gOfficeWin)) {
        CHAR16 *buf = gOfficeDocBuf; UINTN *len = &gOfficeDocLen; UINTN cap = OFFICE_DOC_MAX;
        if (gOfficeTab == 1) { buf = gOfficeSheetBuf; len = &gOfficeSheetLen; cap = OFFICE_SHEET_MAX; }
        else if (gOfficeTab == 2) { buf = gOfficeSlideBuf; len = &gOfficeSlideLen; cap = OFFICE_SLIDE_MAX; }
        if (Key.UnicodeChar == 0x08) {
            if (*len > 0) { (*len)--; buf[*len] = L'\0'; }
        } else if (Key.UnicodeChar == 0x0D || Key.UnicodeChar == 0x0A) {
            if (*len < cap-1) { buf[*len] = L'\n'; (*len)++; buf[*len] = L'\0'; }
        } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E && *len < cap-1) {
            buf[*len] = Key.UnicodeChar; (*len)++; buf[*len] = L'\0';
        }
        return;
    }

    if (gB64Win.IsVisible && !gB64Win.IsMinimized && IsTopWindow(&gB64Win)) {
        if (Key.UnicodeChar==0x0D || Key.UnicodeChar==0x0A) B64Encode();
        else if (Key.UnicodeChar==0x08) { if (gB64InputLen>0) { gB64InputLen--; gB64Input[gB64InputLen]=L'\0'; } }
        else if (Key.UnicodeChar>=0x20 && Key.UnicodeChar<=0x7E && gB64InputLen<B64_BUF-1) {
            gB64Input[gB64InputLen++]=Key.UnicodeChar; gB64Input[gB64InputLen]=L'\0';
        }
        return;
    }

    if (gHashWin.IsVisible && !gHashWin.IsMinimized && IsTopWindow(&gHashWin)) {
        if (Key.UnicodeChar == 0x0D || Key.UnicodeChar == 0x0A) HashComputeBoth();
        else if (Key.UnicodeChar == 0x08) { if (gHashInputLen > 0) { gHashInputLen--; gHashInput[gHashInputLen] = L'\0'; } }
        else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E && gHashInputLen < HASH_INPUT_MAX-1) {
            gHashInput[gHashInputLen++] = Key.UnicodeChar;
            gHashInput[gHashInputLen] = L'\0';
        }
        return;
    }

    if (gPianoWin.IsVisible && !gPianoWin.IsMinimized && IsTopWindow(&gPianoWin)) {
        STATIC CONST CHAR16 White[] = L"asdfghj";
        STATIC CONST INT32  WhiteI[] = {0,2,4,5,7,9,11};
        STATIC CONST CHAR16 Black[] = L"wetyu";
        STATIC CONST INT32  BlackI[] = {1,3,6,8,10};
        if (Key.UnicodeChar) {
            for (INT32 i = 0; i < 7; i++)
                if (Key.UnicodeChar == White[i] || Key.UnicodeChar == (White[i]-32)) { PianoPlayKey(WhiteI[i]); return; }
            for (INT32 i = 0; i < 5; i++)
                if (Key.UnicodeChar == Black[i] || Key.UnicodeChar == (Black[i]-32)) { PianoPlayKey(BlackI[i]); return; }
        }
        return;
    }

    if (g2048Win.IsVisible && !g2048Win.IsMinimized && IsTopWindow(&g2048Win)) {
        if (Key.ScanCode==SCAN_UP)    G2048Move(0,-1);
        if (Key.ScanCode==SCAN_DOWN)  G2048Move(0, 1);
        if (Key.ScanCode==SCAN_LEFT)  G2048Move(-1,0);
        if (Key.ScanCode==SCAN_RIGHT) G2048Move(1, 0);
        if (g2048Score > g2048Best) g2048Best = g2048Score;
        return;
    }

    if (gPongWin.IsVisible && !gPongWin.IsMinimized && IsTopWindow(&gPongWin)) {
        if (Key.UnicodeChar==L'w' || Key.UnicodeChar==L'W' || Key.ScanCode==SCAN_UP) {
            gPongPlayerY -= 14; if (gPongPlayerY<0) gPongPlayerY=0;
        }
        if (Key.UnicodeChar==L's' || Key.UnicodeChar==L'S' || Key.ScanCode==SCAN_DOWN) {
            gPongPlayerY += 14; if (gPongPlayerY>PONG_H-PONG_PH) gPongPlayerY=PONG_H-PONG_PH;
        }
        return;
    }

    if (gTetrisWin.IsVisible && !gTetrisWin.IsMinimized && IsTopWindow(&gTetrisWin)) {
        if (Key.ScanCode == SCAN_LEFT)  { TetrisMove(-1); return; }
        if (Key.ScanCode == SCAN_RIGHT) { TetrisMove(1);  return; }
        if (Key.ScanCode == SCAN_UP)    { TetrisRotate(); return; }
        if (Key.ScanCode == SCAN_DOWN) {
            if (!TetrisCollision(gTetrisX,gTetrisY+1,gTetrisPiece,gTetrisRotation)) gTetrisY++;
            return;
        }
        return;
    }

    if (gNotesWin.IsVisible && !gNotesWin.IsMinimized && IsTopWindow(&gNotesWin)) {
        if (Key.UnicodeChar == 0x08) {
            if (gNotesLen > 0) { gNotesLen--; gNotesBuffer[gNotesLen] = L'\0'; gNotesDirty = TRUE; }
        } else if (Key.UnicodeChar == 0x0D || Key.UnicodeChar == 0x0A) {
            if (gNotesLen < NOTES_MAX_TEXT - 1) { gNotesBuffer[gNotesLen++] = L'\n'; gNotesBuffer[gNotesLen] = L'\0'; gNotesDirty = TRUE; }
        } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E && gNotesLen < NOTES_MAX_TEXT - 1) {
            gNotesBuffer[gNotesLen++] = Key.UnicodeChar;
            gNotesBuffer[gNotesLen] = L'\0';
            gNotesDirty = TRUE;
        }
        return;
    }

    if (gMiniIDEWin.IsVisible && !gMiniIDEWin.IsMinimized && IsTopWindow(&gMiniIDEWin)) {
        if (Key.UnicodeChar == 0x08) {
            if (gIDELen > 0) { gIDELen--; gIDEBuffer[gIDELen] = L'\0'; gIDECursor = gIDELen; gIDEDirty = TRUE; }
        } else if (Key.UnicodeChar == 0x0D || Key.UnicodeChar == 0x0A) {
            if (gIDELen < IDE_MAX_TEXT - 1) { gIDEBuffer[gIDELen++] = L'\n'; gIDEBuffer[gIDELen] = L'\0'; gIDECursor = gIDELen; gIDEDirty = TRUE; }
        } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E && gIDELen < IDE_MAX_TEXT - 1) {
            gIDEBuffer[gIDELen++] = Key.UnicodeChar;
            gIDEBuffer[gIDELen] = L'\0';
            gIDECursor = gIDELen;
            gIDEDirty = TRUE;
        }
        return;
    }

    if (gUnitConverterWin.IsVisible && !gUnitConverterWin.IsMinimized && IsTopWindow(&gUnitConverterWin)) {
        if (Key.UnicodeChar == 0x08) {
            if (gUnitInputLen > 0) { gUnitInputLen--; gUnitInput[gUnitInputLen] = L'\0'; }
        } else if (Key.UnicodeChar >= L'0' && Key.UnicodeChar <= L'9' && gUnitInputLen < 31) {
            gUnitInput[gUnitInputLen++] = Key.UnicodeChar;
            gUnitInput[gUnitInputLen] = L'\0';
        }
        return;
    }

    if (gSearchWin.IsVisible && !gSearchWin.IsMinimized && IsTopWindow(&gSearchWin)) {
        if (Key.UnicodeChar == 0x08) {
            if (gSearchQueryLen > 0) { gSearchQueryLen--; gSearchQuery[gSearchQueryLen] = L'\0'; gSearchDone = FALSE; }
        } else if (Key.UnicodeChar >= 0x20 && Key.UnicodeChar <= 0x7E && gSearchQueryLen < 63) {
            gSearchQuery[gSearchQueryLen++] = Key.UnicodeChar;
            gSearchQuery[gSearchQueryLen] = L'\0';
            gSearchDone = FALSE;
        }
        return;
    }

    if (gPaintTypingText) {
        if (Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A) {
            for (UINTN i=0;i<gPaintTextLen;i++) {
                INT32 px=gPaintTextTargetX+(INT32)i*CharW();
                INT32 py=gPaintTextTargetY;
                for (INT32 r=0;r<8;r++) {
                    UINT8 row=FONT_8x8[gPaintTextBuf[i]>127?'?':gPaintTextBuf[i]][r];
                    for (INT32 c=0;c<8;c++)
                        if (row&(1<<(7-c))) {
                            INT32 tx=px+c,ty=py+r;
                            if (tx>=0&&tx<PAINT_W&&ty>=0&&ty<PAINT_H) gPaintCanvas[ty*PAINT_W+tx]=gCurrentColor;
                        }
                }
            }
            gPaintTypingText=FALSE; gPaintTextBuf[0]=L'\0'; gPaintTextLen=0;
        } else if (Key.UnicodeChar==0x08&&gPaintTextLen>0) {
            gPaintTextBuf[--gPaintTextLen]=L'\0';
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gPaintTextLen<31) {
            gPaintTextBuf[gPaintTextLen++]=Key.UnicodeChar;
            gPaintTextBuf[gPaintTextLen]=L'\0';
        }
        return;
    }

    if (gInTextTermEditor) {
        if (Key.ScanCode==SCAN_ESC) { SaveTextTermFile(); gInTextTermEditor=FALSE; }
        else if (Key.UnicodeChar==0x08&&gTextTermLen>0) {
            gTextTermBuffer[--gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"Editing... [at save time: Unsaved]");
        } else if ((Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A)&&gTextTermLen<MAX_TEXT-1) {
            gTextTermBuffer[gTextTermLen++]=L'\n'; gTextTermBuffer[gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"Editing... [at save time: Unsaved]");
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gTextTermLen<MAX_TEXT-1) {
            gTextTermBuffer[gTextTermLen++]=Key.UnicodeChar; gTextTermBuffer[gTextTermLen]=L'\0';
            StrCpyS(gTextTermStatus,64,L"Editing... [at save time: Unsaved]");
        }
        return;
    }

    if (gGuiMatrixActive &&
        !gInShellMode &&
        !gInTextTermEditor &&
        gTextTermWin.IsVisible &&
        !gTextTermWin.IsMinimized &&
        IsTopWindow(&gTextTermWin)) {

        if (Key.ScanCode == SCAN_ESC ||
            Key.UnicodeChar == 0x1B) {
            gGuiMatrixActive = FALSE;
            TerminalPrint(L"Matrix stopped.");
        }

        gRedrawWindows = TRUE;
        return;
    }

    if ((gInShellMode ||
         (gTextTermWin.IsVisible &&
          !gTextTermWin.IsMinimized &&
          IsTopWindow(&gTextTermWin))) &&
        !gInTextTermEditor) {

        if (gInShellMode && gShellHelpActive) {
            if (Key.ScanCode == SCAN_ESC) {
                gShellHelpActive = FALSE;
            } else if (Key.UnicodeChar == L' ') {
                if (gHelpScroll + SHELL_HELP_LINES_PER_PAGE < HELP_LINE_COUNT)
                    gHelpScroll += SHELL_HELP_LINES_PER_PAGE;
            } else if (Key.UnicodeChar == 0x08) {
                if (gHelpScroll >= SHELL_HELP_LINES_PER_PAGE)
                    gHelpScroll -= SHELL_HELP_LINES_PER_PAGE;
            }

            gRedrawWindows = TRUE;
            return;
        }

        if (Key.UnicodeChar==0x0D || Key.UnicodeChar==0x0A) {
            if (gInShellMode)
                ExecuteShellCommand();
            else
                ExecuteCommandString(gShellCmdBuf,FALSE);

            gShellCmdBuf[0]=L'\0';
            gShellCmdLen=0;

        } else if (Key.UnicodeChar==0x08 && gShellCmdLen>0) {
            gShellCmdBuf[--gShellCmdLen]=L'\0';

        } else if (Key.UnicodeChar>=0x20 &&
                   Key.UnicodeChar<=0x7E &&
                   gShellCmdLen<127) {
            gShellCmdBuf[gShellCmdLen++]=Key.UnicodeChar;
            gShellCmdBuf[gShellCmdLen]=L'\0';
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
        if (Key.UnicodeChar==0x08&&*TargetLen>0) TargetBuf[--(*TargetLen)]=L'\0';
        else if (Key.UnicodeChar==0x09) gActiveField=!gActiveField;
        else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&*TargetLen<31) {
            TargetBuf[(*TargetLen)++]=Key.UnicodeChar;
            TargetBuf[*TargetLen]=L'\0';
        }
        return;
    }

    if (gEditorWin.IsVisible) {
        if (Key.UnicodeChar==0x08&&gTextLen>0) gTextBuffer[--gTextLen]=L'\0';
        else if (Key.UnicodeChar==0x09) {
            for (INT32 k=0;k<4&&gTextLen<MAX_TEXT-1;k++) gTextBuffer[gTextLen++] = L' ';
            gTextBuffer[gTextLen]=L'\0';
        } else if ((Key.UnicodeChar==0x0D||Key.UnicodeChar==0x0A)&&gTextLen<MAX_TEXT-1) {
            gTextBuffer[gTextLen++]=L'\n'; gTextBuffer[gTextLen]=L'\0';
        } else if (Key.UnicodeChar>=0x20&&Key.UnicodeChar<=0x7E&&gTextLen<MAX_TEXT-1) {
            gTextBuffer[gTextLen++]=Key.UnicodeChar; gTextBuffer[gTextLen]=L'\0';
        }
    }
}

VOID RenderTaskbarAndStartMenu(VOID) {
    INT32 TaskbarY = (INT32)gScreenHeight - TASKBAR_H;
    DrawRect(0,TaskbarY,gScreenWidth,TASKBAR_H,COLOR_TOPBAR_BG);
    DrawButton(4,TaskbarY+4,TASKBAR_START_W,TASKBAR_BUTTON_H,L"Start",
        gStartMenuOpen?COLOR_TITLE_ACTIVE:COLOR_TOOLBAR_BG,COLOR_TEXT);

    INT32 LeftArrowX = TASKBAR_START_W + 8;
    INT32 WindowAreaX = LeftArrowX + TASKBAR_SCROLL_W + 4;
    INT32 RightArrowX = WindowAreaX + (INT32)GetTaskbarVisibleCount() * TASKBAR_BUTTON_W + 4;
    DrawButton(LeftArrowX,TaskbarY+4,TASKBAR_SCROLL_W,TASKBAR_BUTTON_H,L"<",COLOR_TOOLBAR_BG,COLOR_TEXT);
    DrawButton(RightArrowX,TaskbarY+4,TASKBAR_SCROLL_W,TASKBAR_BUTTON_H,L">",COLOR_TOOLBAR_BG,COLOR_TEXT);

    UINTN VisibleCount = GetTaskbarVisibleCount();
    UINTN VisibleDrawn = 0;
    UINTN SkipDrawn = 0;
    for (UINTN i = 0; i < APP_COUNT; i++) {
        APP_REG *R = &gAppRegistry[i];
        if (!R->InBar || !R->Win || !R->Win->IsVisible) continue;
        if (SkipDrawn < gTaskbarScroll) { SkipDrawn++; continue; }
        if (VisibleDrawn >= VisibleCount) break;
        INT32 ButtonX = WindowAreaX + (INT32)VisibleDrawn * TASKBAR_BUTTON_W;
        BOOLEAN IsActive = !R->Win->IsMinimized && IsTopWindow(R->Win);
        DrawButton(ButtonX, TaskbarY+4, TASKBAR_BUTTON_W-4, TASKBAR_BUTTON_H, R->BarName,
            IsActive ? COLOR_TITLE_ACTIVE : COLOR_TOOLBAR_BG, COLOR_TEXT);
        VisibleDrawn++;
    }

    INT32 ClockX = (INT32)gScreenWidth - TASKBAR_POWER_W - TASKBAR_CLOCK_W - 8;
    {
        CHAR16 FpsBuf[32];
        UnicodeSPrint(FpsBuf, sizeof(FpsBuf), L"FPS:%u R:%u", gFps, gWallpaperRebuilds);
        INT32 FpsX = ClockX;
        DrawRect(FpsX, TaskbarY+4, TASKBAR_CLOCK_W, TASKBAR_BUTTON_H, COLOR_TOOLBAR_BG);
        DrawString(FpsBuf, FpsX+6, TaskbarY+12,
            gFps >= 55 ? NEW_APP_COLOR_GOOD :
            (gFps >= 30 ? NEW_APP_COLOR_WARN : COLOR_BTN_CLOSE));
    }

    DrawButton((INT32)gScreenWidth-TASKBAR_POWER_W-4,TaskbarY+4,TASKBAR_POWER_W,TASKBAR_BUTTON_H,L"Power",COLOR_BTN_CLOSE,COLOR_TEXT);

    if (gStartMenuOpen) {
        INT32 MenuW = 430, MenuH = 300;
        INT32 MenuX = 6, MenuY = TaskbarY - MenuH - 4;
        DrawRect(MenuX,MenuY,MenuW,MenuH,COLOR_WIN_BG);
        DrawRect(MenuX,MenuY,MenuW,22,COLOR_TITLE_ACTIVE);
        DrawString(L"Applications",MenuX+10,MenuY+7,COLOR_TEXT);

        INT32 VisibleRows = 12, VisibleCols = 2;

        UINTN MenuCount = 0;
        for (UINTN i = 0; i < APP_COUNT; i++)
            if (gAppRegistry[i].InMenu && gAppRegistry[i].Win) MenuCount++;

        UINTN TotalRows = (MenuCount + VisibleCols - 1) / VisibleCols;
        UINTN MaxScroll = TotalRows > (UINTN)VisibleRows ? TotalRows - VisibleRows : 0;
        if (gStartMenuScroll > MaxScroll) gStartMenuScroll = MaxScroll;

        for (UINTN r = 0; r < (UINTN)VisibleRows; r++) {
            for (INT32 c = 0; c < VisibleCols; c++) {
                UINTN want = (r + gStartMenuScroll) * VisibleCols + c;
                UINTN seen = 0;
                APP_REG *pick = NULL;
                for (UINTN i = 0; i < APP_COUNT; i++) {
                    if (!gAppRegistry[i].InMenu || !gAppRegistry[i].Win) continue;
                    if (seen++ == want) { pick = &gAppRegistry[i]; break; }
                }
                if (!pick) break;
                INT32 BX = MenuX + 6 + c * 210;
                INT32 BY = MenuY + 26 + (INT32)r * 19;
                DrawButton(BX, BY, 198, 18, pick->Win->Title, COLOR_TOOLBAR_BG, COLOR_TEXT);
            }
        }

        DrawButton(MenuX + 6,          MenuY + MenuH - 22, 90, 18, L"Up",
            gStartMenuScroll == 0 ? COLOR_WIN_BORDER : COLOR_TITLE_ACTIVE, COLOR_TEXT);
        DrawButton(MenuX + MenuW - 96, MenuY + MenuH - 22, 90, 18, L"Down",
            gStartMenuScroll >= MaxScroll ? COLOR_WIN_BORDER : COLOR_TITLE_ACTIVE, COLOR_TEXT);
    }

    if (gShowPowerDialog) {
        INT32 DiaW = 320, DiaH = 160;
        INT32 DiaX = ((INT32)gScreenWidth - DiaW) / 2;
        INT32 DiaY = ((INT32)gScreenHeight - DiaH) / 2;
        DrawRect(DiaX,DiaY,DiaW,DiaH,COLOR_WIN_BG);
        DrawRect(DiaX,DiaY,DiaW,24,COLOR_TITLE_ACTIVE);
        DrawString(L"Power popup",DiaX+10,DiaY+6,COLOR_TEXT);
        DrawString(L"Select a power operation:",DiaX+15,DiaY+34,COLOR_TEXT);
        DrawButton(DiaX+20,DiaY+55,80,30,L"Poweroff",COLOR_BTN_CLOSE,COLOR_TEXT);
        DrawButton(DiaX+120,DiaY+55,80,30,L"Reboot",COLOR_TOOLBAR_BG,COLOR_TEXT);
        DrawButton(DiaX+220,DiaY+55,80,30,L"Shell",NEW_APP_COLOR_GOOD,COLOR_TEXT);
        DrawButton(DiaX+110,DiaY+105,100,25,L"Cancel",COLOR_WIN_BORDER,COLOR_TEXT);
    }
}

VOID DrawMouseCursor(INT32 x,INT32 y) {
    for (INT32 r=0;r<16;r++) for (INT32 c=0;c<16;c++) {
        UINT8 pixel=MOUSE_CURSOR_16x16[r][c];
        if (pixel==1) FastDrawPixel(x+c,y+r,RGB(0,0,0));
        else if (pixel==2) FastDrawPixel(x+c,y+r,RGB(255,255,255));
    }
}

VOID ProcessInput(BOOLEAN Click,BOOLEAN LastClick) {
    if (gFileDlgMode != FILEDLG_NONE) return;
    if (Click && !LastClick) gRedrawWindows = TRUE;
    if (!Click) {
        for (UINTN i=0;i<gWindowCount;i++)
            if (gWinOrder[i]) gWinOrder[i]->IsDragging=FALSE;
    } else {
        for (UINTN i=0;i<gWindowCount;i++)
            if (gWinOrder[i]&&gWinOrder[i]->IsDragging) {
                INT32 nx = gMouseX - gWinOrder[i]->DragOffsetX;
                INT32 ny = gMouseY - gWinOrder[i]->DragOffsetY;
                if (nx != gWinOrder[i]->X || ny != gWinOrder[i]->Y) gDesktopDirty = TRUE;
                gWinOrder[i]->X = nx;
                gWinOrder[i]->Y = ny;
            }
    }
    
        if (Click && LastClick &&
        gSandboxWin.IsVisible &&
        !gSandboxWin.IsMinimized &&
        !gSandboxWin.IsDragging &&
        IsTopWindow(&gSandboxWin) &&
        gSandboxWin.Input) {
        gSandboxWin.Input(
            &gSandboxWin, gMouseX, gMouseY, TRUE, TRUE
        );
    }
    if (!Click||LastClick) return;

    INT32 TaskbarY = (INT32)gScreenHeight - TASKBAR_H;

    if (gShowPowerDialog) {
        INT32 DiaW=320,DiaH=160;
        INT32 DiaX=((INT32)gScreenWidth-DiaW)/2;
        INT32 DiaY=((INT32)gScreenHeight-DiaH)/2;
        if (IsInRect(gMouseX,gMouseY,DiaX+20,DiaY+55,80,30)) ShowShutdownSequence(FALSE);
        else if (IsInRect(gMouseX,gMouseY,DiaX+120,DiaY+55,80,30)) ShowShutdownSequence(TRUE);
        else if (IsInRect(gMouseX,gMouseY,DiaX+220,DiaY+55,80,30)) { gInShellMode=TRUE; gShowPowerDialog=FALSE; gStartMenuOpen=FALSE; }
        else if (IsInRect(gMouseX,gMouseY,DiaX+110,DiaY+105,100,25)) gShowPowerDialog=FALSE;
        return;
    }

    if (gStartMenuOpen) {
        INT32 MenuW = 430, MenuH = 300;
        INT32 MenuX = 6, MenuY = TaskbarY - MenuH - 4;
        INT32 VisibleRows = 12, VisibleCols = 2;

        UINTN MenuCount = 0;
        for (UINTN i = 0; i < APP_COUNT; i++)
            if (gAppRegistry[i].InMenu && gAppRegistry[i].Win) MenuCount++;

        UINTN TotalRows = (MenuCount + VisibleCols - 1) / VisibleCols;
        UINTN MaxScroll = TotalRows > (UINTN)VisibleRows ? TotalRows - VisibleRows : 0;

        if (IsInRect(gMouseX,gMouseY,MenuX+6,MenuY+MenuH-22,90,18)) {
            if (gStartMenuScroll > 0) gStartMenuScroll--;
            return;
        }
        if (IsInRect(gMouseX,gMouseY,MenuX+MenuW-96,MenuY+MenuH-22,90,18)) {
            if (gStartMenuScroll < MaxScroll) gStartMenuScroll++;
            return;
        }
        if (IsInRect(gMouseX,gMouseY,MenuX,MenuY,MenuW,MenuH - 22)) {
            for (UINTN r = 0; r < (UINTN)VisibleRows; r++) {
                for (INT32 c = 0; c < VisibleCols; c++) {
                    UINTN want = (r + gStartMenuScroll) * VisibleCols + c;
                    UINTN seen = 0;
                    APP_REG *pick = NULL;
                    for (UINTN i = 0; i < APP_COUNT; i++) {
                        if (!gAppRegistry[i].InMenu || !gAppRegistry[i].Win) continue;
                        if (seen++ == want) { pick = &gAppRegistry[i]; break; }
                    }
                    if (!pick) break;
                    INT32 BX = MenuX + 6 + c * 210;
                    INT32 BY = MenuY + 26 + (INT32)r * 19;
                    if (IsInRect(gMouseX,gMouseY,BX,BY,198,18)) {
                        pick->Win->IsVisible = TRUE;
                        pick->Win->IsMinimized = FALSE;
                        if (pick->Win == &gSettingsWin) gSettingSection = 0;
                        BringToFront(pick->Win);
                        gStartMenuOpen = FALSE;
                        gDesktopDirty = TRUE;
                        return;
                    }
                }
            }
        } else if (!IsInRect(gMouseX,gMouseY,6,TaskbarY+4,60,24)) {
            gStartMenuOpen = FALSE;
            gDesktopDirty = TRUE;
        }
    }

    if (IsInRect(gMouseX,gMouseY,0,TaskbarY,gScreenWidth,TASKBAR_H)) {
        if (IsInRect(gMouseX,gMouseY,4,TaskbarY+4,TASKBAR_START_W,TASKBAR_BUTTON_H)) {
            gStartMenuOpen = !gStartMenuOpen;
            if (gStartMenuOpen) gStartMenuScroll = 0;
            gDesktopDirty = TRUE;
            return;
        }
        INT32 LeftArrowX = TASKBAR_START_W + 8;
        INT32 WindowAreaX = LeftArrowX + TASKBAR_SCROLL_W + 4;
        if (IsInRect(gMouseX,gMouseY,LeftArrowX,TaskbarY+4,TASKBAR_SCROLL_W,TASKBAR_BUTTON_H)) {
            if (gTaskbarScroll > 0) gTaskbarScroll--;
            return;
        }
        UINTN VisibleCount = GetTaskbarVisibleCount();
        UINTN VisibleIndex = 0;
        UINTN SkipDrawn = 0;
        for (UINTN i = 0; i < APP_COUNT; i++) {
            APP_REG *R = &gAppRegistry[i];
            if (!R->InBar || !R->Win || !R->Win->IsVisible) continue;
            if (SkipDrawn < gTaskbarScroll) { SkipDrawn++; continue; }
            if (VisibleIndex >= VisibleCount) break;
            INT32 ButtonX = WindowAreaX + (INT32)VisibleIndex * TASKBAR_BUTTON_W;
            if (IsInRect(gMouseX,gMouseY,ButtonX,TaskbarY+4,TASKBAR_BUTTON_W-4,TASKBAR_BUTTON_H)) {
                UEFI_WINDOW *W = R->Win;
                if (W->IsMinimized) { W->IsMinimized = FALSE; BringToFront(W); }
                else if (IsTopWindow(W)) { W->IsMinimized = TRUE; gDesktopDirty = TRUE; }
                else BringToFront(W);
                return;
            }
            VisibleIndex++;
        }
        INT32 RightArrowX = WindowAreaX + (INT32)VisibleCount * TASKBAR_BUTTON_W + 4;
        if (IsInRect(gMouseX,gMouseY,RightArrowX,TaskbarY+4,TASKBAR_SCROLL_W,TASKBAR_BUTTON_H)) {
            UINTN BarTotal = 0;
            for (UINTN i = 0; i < APP_COUNT; i++)
                if (gAppRegistry[i].InBar && gAppRegistry[i].Win && gAppRegistry[i].Win->IsVisible) BarTotal++;
            if (gTaskbarScroll + VisibleCount < BarTotal) gTaskbarScroll++;
            return;
        }
        INT32 PowerX = (INT32)gScreenWidth - TASKBAR_POWER_W - 4;
        if (IsInRect(gMouseX,gMouseY,PowerX,TaskbarY+4,TASKBAR_POWER_W,TASKBAR_BUTTON_H)) {
            gShowPowerDialog = TRUE;
            return;
        }
        return;
    }

    for (INT32 i=(INT32)gWindowCount-1;i>=0;i--) {
        UEFI_WINDOW *Win=gWinOrder[i];
        if (!Win||!Win->IsVisible||Win->IsMinimized) continue;
        if (IsInRect(gMouseX,gMouseY,Win->X,Win->Y,Win->W,26)) {
            BringToFront(Win);
            if (HasCustomChrome(Win)) {
                if (Win->Input) Win->Input(Win, gMouseX, gMouseY, Click, LastClick);
                if (Win->IsVisible) {
                    Win->IsDragging = TRUE;
                    Win->DragOffsetX = gMouseX - Win->X;
                    Win->DragOffsetY = gMouseY - Win->Y;
                }
                return;
            }
            INT32 BtnW=24;
            INT32 CloseX=Win->X+Win->W-BtnW;
            INT32 MaxX=CloseX-BtnW;
            INT32 MinX=MaxX-BtnW;
            if (IsInRect(gMouseX,gMouseY,CloseX,Win->Y,BtnW,26)) { Win->IsVisible=FALSE; gDesktopDirty=TRUE; }
            else if (IsInRect(gMouseX,gMouseY,MaxX,Win->Y,BtnW,26)) { ToggleWindowMaximize(Win); gDesktopDirty=TRUE; }
            else if (IsInRect(gMouseX,gMouseY,MinX,Win->Y,BtnW,26)) { Win->IsMinimized=TRUE; gDesktopDirty=TRUE; }
            else { Win->IsDragging=TRUE; Win->DragOffsetX=gMouseX-Win->X; Win->DragOffsetY=gMouseY-Win->Y; }
            return;
        }
        if (IsInRect(gMouseX,gMouseY,Win->X,Win->Y+26,Win->W,Win->H-26)) {
            BringToFront(Win);
            if (Win->Input) Win->Input(Win,gMouseX,gMouseY,Click,LastClick);
            return;
        }
    }
}

EFI_STATUS EFIAPI UefiMain(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID**)&gGop);
    if (EFI_ERROR(Status)) return Status;
    gScreenWidth=gGop->Mode->Info->HorizontalResolution;
    gScreenHeight=gGop->Mode->Info->VerticalResolution;
    gPixelsPerScanLine=gGop->Mode->Info->PixelsPerScanLine;
    gFrameBuffer=(UINT32*)(UINTN)gGop->Mode->FrameBufferBase;
    UINTN FrameBufferSize=gPixelsPerScanLine*gScreenHeight*sizeof(UINT32);
    Status=gBS->AllocatePool(EfiLoaderData,FrameBufferSize,(VOID**)&gBackBuffer);
    if (EFI_ERROR(Status)) gBackBuffer=gFrameBuffer;

    ResetClip();

    EFI_HANDLE *PointerHandles=NULL;
    UINTN NumHandles=0;
    Status=gBS->LocateHandleBuffer(ByProtocol,&gEfiSimplePointerProtocolGuid,NULL,&NumHandles,&PointerHandles);
    if (!EFI_ERROR(Status)) {
        for (UINTN i=0;i<NumHandles&&gNumMouseDevices<MAX_POINTER_DEVICES;i++) {
            EFI_SIMPLE_POINTER_PROTOCOL *Ptr=NULL;
            if (!EFI_ERROR(gBS->HandleProtocol(PointerHandles[i],&gEfiSimplePointerProtocolGuid,(VOID**)&Ptr))) {
                gMouseDevices[gNumMouseDevices++]=Ptr;
                Ptr->Reset(Ptr,TRUE);
            }
        }
        gBS->FreePool(PointerHandles);
    }
    gBS->LocateProtocol(&gEfiAbsolutePointerProtocolGuid,NULL,(VOID**)&gAbsMouse);
    if (gAbsMouse) gAbsMouse->Reset(gAbsMouse,TRUE);

    CalibrateTsc();
    InitWindowRegistry();
    ResetSnakeGame();
    ResetTetris();
    G2048Reset();
    MSReset();
    PongResetBall(1);
    SolNewGame();
    RefreshFileList();
    StoreInit();
    StoreResizeWindow();
    ShowStartupScreen();

    BOOLEAN LastLeftClick=FALSE;

    while (TRUE) {
        BOOLEAN LeftClick=FALSE;
        if (gFileDlgMode == FILEDLG_NONE) {
            PollNewAppsKeyboard();
            PollKeyboard();
        }
        PollMouse(&LeftClick);
        if (gFileDlgMode != FILEDLG_NONE) FileDialogHandleInput(LeftClick, LastLeftClick);

        if (gJumpscareActive && gJumpscareSoundPending) {
            gJumpscareSoundPending = FALSE;
            PlayJumpscareBeep();
        }
        if (gJumpscareActive || gJumpscareMessage) UpdateJumpscare();

        RestoreUnderCursor();

        if (gJumpscareActive) { ResetClip(); RenderJumpscare(); }
        else if (gJumpscareMessage) { ResetClip(); RenderJumpscareMessage(); }
        else if (gInShellMode) { ResetClip(); RenderShellMode(); }
        else {
            ResetClip();
            BOOLEAN NeedFullRedraw = gDesktopDirty;
            BOOLEAN ForceRedraw = gRedrawWindows || NeedFullRedraw;
            gRedrawWindows = FALSE;
			BOOLEAN WallpaperWillRebuild = gWallpaperCacheDirty;
			RenderNewWallpaper();

			if (WallpaperWillRebuild)
				ForceRedraw = TRUE;

			/* Repaint all windows over the animated Sandbox. */
			if (gSandboxWin.IsVisible &&
				!gSandboxWin.IsMinimized &&
				!gSandboxPaused) {
				ForceRedraw = TRUE;
			}

			/* Keep updating particles even when other windows cover Sandbox. */
			UpdateSandbox();			
            if (gSnakeWin.IsVisible&&!gSnakeWin.IsMinimized) {
                gSnakeTick++;
                if (gSnakeTick%5==0) UpdateSnakeGame();
            }

            if (gTetrisWin.IsVisible && !gTetrisWin.IsMinimized && !gTetrisGameOver) {
                EFI_TIME TetrisTime;
                if (!EFI_ERROR(gRT->GetTime(&TetrisTime, NULL))) {
                    if (TetrisTime.Second != gTetrisLastSecond) {
                        gTetrisLastSecond = TetrisTime.Second;
                        UpdateTetris();
                    }
                }
            }

            if (gPongWin.IsVisible && !gPongWin.IsMinimized && !gPongGameOver) {
                UpdatePong();
            }

            if (gStopwatchRunning) {
                EFI_TIME StopwatchTime;
                if (!EFI_ERROR(gRT->GetTime(&StopwatchTime,NULL))) {
                    if (StopwatchTime.Second != gStopwatchLastSecond) {
                        gStopwatchLastSecond = StopwatchTime.Second;
                        gStopwatchTicks++;
                        if (gStopwatchTicks > 359999) gStopwatchTicks = 0;
                    }
                }
            }

            if (gAnimPlaying && gSettingAnimations) {
                gAnimTickCount++;
                if (gAnimTickCount>=gAnimSpeedDelay) {
                    gAnimTickCount=0;
                    gViewFrameIdx=(gViewFrameIdx+1)%gAnimFrameCount;
                }
            }

            UpdatePianoTick();
            UpdateScarletFire();

            for (UINTN i = 0; i < gWindowCount; i++) {
                UEFI_WINDOW *Win = gWinOrder[i];
                if (Win && Win->IsVisible && !Win->IsMinimized) {
				BOOLEAN Animated =
					(Win == &gSnakeWin) ||
					(Win == &gTetrisWin) ||
					(Win == &gPongWin) ||
					(Win == &gAnimViewWin) ||
					(Win == &gMusicWin && gMusicPlaying) ||
					(Win == &gTextTermWin && gGuiMatrixActive);
                    if (Animated || ForceRedraw) {
                        UINTN SavedScale = gFontScale;
                        if (Win->IsMaximized && gFontScale < 2) gFontScale = 2;
                        SetClip(Win->X, Win->Y, Win->W, Win->H);
                        Win->Render(Win);
                        ResetClip();
                        gFontScale = SavedScale;
                    }
                }
            }

            ProcessInput(LeftClick,LastLeftClick);

            if (gWallpaperAnimate && gSettingAnimations &&
                (gWallpaperMode == 4 || gWallpaperMode == 5 ||
                 gWallpaperMode == 8 || gWallpaperMode == 9 ||
                 gWallpaperMode == 10)) {
                gWallpaperPhase += 3;
                gWallpaperCacheDirty = TRUE;
            }

            RenderTaskbarAndStartMenu();
            FileDialogRender();
            SaveUnderCursor(gMouseX, gMouseY);
            DrawMouseCursor(gMouseX,gMouseY);
        }

        SwapBuffers();
        LastLeftClick=LeftClick;

        FrameDelayTsc(gFrameDelayUs);

        gFrameCounter++;
        {
            UINT64 now = read_tsc();
            if (gTscPerUs > 0 &&
                (now - gFpsWindowStart) >= (gTscPerUs * 1000000ULL)) {
                gFps = gFrameCounter;
                gFrameCounter = 0;
                gFpsWindowStart = now;
            }
        }
    }

    return EFI_SUCCESS;
}
