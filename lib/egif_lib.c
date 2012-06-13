/******************************************************************************

egif_lib.c - the kernel of the GIF Encoding process can be found here.

*****************************************************************************/

#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>

#if (defined (__MSDOS__) || defined(WINDOWS32))  && !defined(__DJGPP__) && !defined(__GNUC__)
#include <io.h>
#include <sys\stat.h>
#else
#include <sys/types.h>
#include <sys/stat.h>
#endif /* __MSDOS__ */

#include "gif_lib.h"
#include "gif_lib_private.h"

/* Masks given codes to BitsPerPixel, to make sure all codes are in range: */
/*@+charint@*/
static const GifPixelType CodeMask[] = {
    0x00, 0x01, 0x03, 0x07, 0x0f, 0x1f, 0x3f, 0x7f, 0xff
};
/*@-charint@*/

static char GifVersionPrefix[GIF_STAMP_LEN + 1];

#define WRITE(_gif,_buf,_len)   \
  (((GifFilePrivateType*)_gif->Private)->Write ?    \
   ((GifFilePrivateType*)_gif->Private)->Write(_gif,_buf,_len) :    \
   fwrite(_buf, 1, (size_t)_len, ((GifFilePrivateType*)_gif->Private)->File))

static int EGifPutWord(int Word, GifFileType * GifFile);
static int EGifSetupCompress(GifFileType * GifFile);
static int EGifCompressLine(GifFileType * GifFile, GifPixelType * Line,
                            int LineLen);
static int EGifCompressOutput(GifFileType * GifFile, int Code);
static int EGifBufferedOutput(GifFileType * GifFile, GifByteType * Buf,
                              int c);

/******************************************************************************
 * Open a new gif file for write, given by its name. If TestExistance then
 * if the file exists this routines fails (returns NULL).
 * Returns GifFileType pointer dynamically allocated which serves as the gif
 * info record. _GifError is cleared if succesfull.
 *****************************************************************************/
GifFileType *
EGifOpenFileName(const char *FileName, const bool TestExistence)
{

    int FileHandle;
    GifFileType *GifFile;

    if (TestExistence)
        FileHandle = open(FileName, O_WRONLY | O_CREAT | O_EXCL, 
			  S_IREAD | S_IWRITE);
    else
        FileHandle = open(FileName, O_WRONLY | O_CREAT | O_TRUNC, 
			  S_IREAD | S_IWRITE);

    if (FileHandle == -1) {
        _GifError = E_GIF_ERR_OPEN_FAILED;
        return NULL;
    }
    GifFile = EGifOpenFileHandle(FileHandle);
    if (GifFile == (GifFileType *) NULL)
        (void)close(FileHandle);
    return GifFile;
}

/******************************************************************************
 * Update a new gif file, given its file handle, which must be opened for
 * write in binary mode.
 * Returns GifFileType pointer dynamically allocated which serves as the gif
 * info record. _GifError is cleared if succesfull.
 *****************************************************************************/
GifFileType *
EGifOpenFileHandle(const int FileHandle)
{
    GifFileType *GifFile;
    GifFilePrivateType *Private;
    FILE *f;

    GifFile = (GifFileType *) malloc(sizeof(GifFileType));
    if (GifFile == NULL) {
        _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }

    memset(GifFile, '\0', sizeof(GifFileType));

    Private = (GifFilePrivateType *)malloc(sizeof(GifFilePrivateType));
    if (Private == NULL) {
        free(GifFile);
        _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }
    if ((Private->HashTable = _InitHashTable()) == NULL) {
        free(GifFile);
        free(Private);
        _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }

#ifdef __MSDOS__
    _setmode(FileHandle, O_BINARY);    /* Make sure it is in binary mode. */
#endif /* __MSDOS__ */

    f = fdopen(FileHandle, "wb");    /* Make it into a stream: */

    GifFile->Private = (void *)Private;
    Private->FileHandle = FileHandle;
    Private->File = f;
    Private->FileState = FILE_STATE_WRITE;

    Private->Write = (OutputFunc) 0;    /* No user write routine (MRB) */
    GifFile->UserData = (void *)NULL;    /* No user write handle (MRB) */

    _GifError = 0;

    return GifFile;
}

/******************************************************************************
 * Output constructor that takes user supplied output function.
 * Basically just a copy of EGifOpenFileHandle. (MRB)
 *****************************************************************************/
GifFileType *
EGifOpen(void *userData, OutputFunc writeFunc)
{
    GifFileType *GifFile;
    GifFilePrivateType *Private;

    GifFile = (GifFileType *)malloc(sizeof(GifFileType));
    if (GifFile == NULL) {
        _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }

    memset(GifFile, '\0', sizeof(GifFileType));

    Private = (GifFilePrivateType *)malloc(sizeof(GifFilePrivateType));
    if (Private == NULL) {
        free(GifFile);
        _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }

    Private->HashTable = _InitHashTable();
    if (Private->HashTable == NULL) {
        free (GifFile);
        free (Private);
        _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
        return NULL;
    }

    GifFile->Private = (void *)Private;
    Private->FileHandle = 0;
    Private->File = (FILE *) 0;
    Private->FileState = FILE_STATE_WRITE;

    Private->Write = writeFunc;    /* User write routine (MRB) */
    GifFile->UserData = userData;    /* User write handle (MRB) */

    _GifError = 0;

    return GifFile;
}

/******************************************************************************
 * Routine to set current GIF version. All files open for write will be
 * using this version until next call to this routine. Version consists of
 * 3 characters as "87a" or "89a". No test is made to validate the version.
 *****************************************************************************/
void
EGifSetGifVersion(const char *Version)
{
    strncpy(GifVersionPrefix + GIF_VERSION_POS, Version, 3);
}

/******************************************************************************
 * Routine to compute the GIF version that will be written on output.
 *****************************************************************************/
char *
EGifGetGifVersion(GifFileType *GifFile)
{
    GifFilePrivateType *Private = (GifFilePrivateType *) GifFile->Private;
    int i, j;

    /* Bulletproofing - always write GIF89 if we need to */
    for (i = 0; i < GifFile->ImageCount; i++) {
        for (j = 0; j < GifFile->SavedImages[i].Leading.ExtensionBlockCount; j++) {
            int function =
               GifFile->SavedImages[i].Leading.ExtensionBlocks[j].Function;

            if (function == COMMENT_EXT_FUNC_CODE
                || function == GRAPHICS_EXT_FUNC_CODE
                || function == PLAINTEXT_EXT_FUNC_CODE
                || function == APPLICATION_EXT_FUNC_CODE)
                Private->gif89 = true;
        }
    }
    for (i = 0; i < GifFile->Trailing.ExtensionBlockCount; i++) {
	int function = GifFile->Trailing.ExtensionBlocks[i].Function;

	if (function == COMMENT_EXT_FUNC_CODE
	    || function == GRAPHICS_EXT_FUNC_CODE
	    || function == PLAINTEXT_EXT_FUNC_CODE
	    || function == APPLICATION_EXT_FUNC_CODE)
	    Private->gif89 = true;
    }
 
    if (GifVersionPrefix[0] != '\0')
	return GifVersionPrefix;
    else if (Private->gif89)
	return GIF89_STAMP;
    else
	return GIF87_STAMP;
}

/******************************************************************************
 * This routine should be called before any other EGif calls, immediately
 * follows the GIF file openning.
 *****************************************************************************/
int
EGifPutScreenDesc(GifFileType *GifFile,
                  const int Width,
                  const int Height,
                  const int ColorRes,
                  const int BackGround,
                  const ColorMapObject *ColorMap)
{
    GifByteType Buf[3];
    GifFilePrivateType *Private = (GifFilePrivateType *) GifFile->Private;
    char *write_version;

    if (Private->FileState & FILE_STATE_SCREEN) {
        /* If already has screen descriptor - something is wrong! */
        _GifError = E_GIF_ERR_HAS_SCRN_DSCR;
        return GIF_ERROR;
    }
    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    /*
     * Older versions of the library didn't compute which version 
     * the image's extension blocks require here, but rather in EGifSpew(),
     * and it was done in a non-thread-safe way. The default for sequential 
     * writes was GIF87 but you could override it with EGifSetGifVersion.
     * This new code lets you shoot yourself in the foot if you really
     * want to, but defaults to the oldest version that will carry the
     * extensions.
     */
    write_version = EGifGetGifVersion(GifFile);

    /* First write the version prefix into the file. */
    if (WRITE(GifFile, (unsigned char *)write_version,
              strlen(write_version)) != strlen(write_version)) {
        _GifError = E_GIF_ERR_WRITE_FAILED;
        return GIF_ERROR;
    }

    GifFile->SWidth = Width;
    GifFile->SHeight = Height;
    GifFile->SColorResolution = ColorRes;
    GifFile->SBackGroundColor = BackGround;
    if (ColorMap) {
        GifFile->SColorMap = GifMakeMapObject(ColorMap->ColorCount,
                                           ColorMap->Colors);
        if (GifFile->SColorMap == NULL) {
            _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
            return GIF_ERROR;
        }
    } else
        GifFile->SColorMap = NULL;

    /*
     * Put the logical screen descriptor into the file:
     */
    /* Logical Screen Descriptor: Dimensions */
    (void)EGifPutWord(Width, GifFile);
    (void)EGifPutWord(Height, GifFile);

    /* Logical Screen Descriptor: Packed Fields */
    /* Note: We have actual size of the color table default to the largest
     * possible size (7+1 == 8 bits) because the decoder can use it to decide
     * how to display the files.
     */
    Buf[0] = (ColorMap ? 0x80 : 0x00) | /* Yes/no global colormap */
             ((ColorRes - 1) << 4) | /* Bits allocated to each primary color */
        (ColorMap ? ColorMap->BitsPerPixel - 1 : 0x07 ); /* Actual size of the
                                                            color table. */
    if (ColorMap != NULL && ColorMap->SortFlag)
	Buf[0] |= 0x08;
    Buf[1] = BackGround;    /* Index into the ColorTable for background color */
    Buf[2] = GifFile->AspectByte;     /* Pixel Aspect Ratio */
    WRITE(GifFile, Buf, 3);

    /* If we have Global color map - dump it also: */
    if (ColorMap != NULL) {
	int i;
        for (i = 0; i < ColorMap->ColorCount; i++) {
            /* Put the ColorMap out also: */
            Buf[0] = ColorMap->Colors[i].Red;
            Buf[1] = ColorMap->Colors[i].Green;
            Buf[2] = ColorMap->Colors[i].Blue;
            if (WRITE(GifFile, Buf, 3) != 3) {
                _GifError = E_GIF_ERR_WRITE_FAILED;
                return GIF_ERROR;
            }
        }
    }

    /* Mark this file as has screen descriptor, and no pixel written yet: */
    Private->FileState |= FILE_STATE_SCREEN;

    return GIF_OK;
}

/******************************************************************************
 * This routine should be called before any attempt to dump an image - any
 * call to any of the pixel dump routines.
 *****************************************************************************/
int
EGifPutImageDesc(GifFileType *GifFile,
                 const int Left,
                 const int Top,
                 const int Width,
                 const int Height,
                 const bool Interlace,
                 const ColorMapObject *ColorMap)
{
    GifByteType Buf[3];
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (Private->FileState & FILE_STATE_IMAGE &&
        Private->PixelCount > 0xffff0000UL) {
        /* If already has active image descriptor - something is wrong! */
        _GifError = E_GIF_ERR_HAS_IMAG_DSCR;
        return GIF_ERROR;
    }
    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }
    GifFile->Image.Left = Left;
    GifFile->Image.Top = Top;
    GifFile->Image.Width = Width;
    GifFile->Image.Height = Height;
    GifFile->Image.Interlace = Interlace;
    if (ColorMap) {
        GifFile->Image.ColorMap = GifMakeMapObject(ColorMap->ColorCount,
                                                ColorMap->Colors);
        if (GifFile->Image.ColorMap == NULL) {
            _GifError = E_GIF_ERR_NOT_ENOUGH_MEM;
            return GIF_ERROR;
        }
    } else {
        GifFile->Image.ColorMap = NULL;
    }

    /* Put the image descriptor into the file: */
    Buf[0] = DESCRIPTOR_INTRODUCER;    /* Image separator character. */
    WRITE(GifFile, Buf, 1);
    (void)EGifPutWord(Left, GifFile);
    (void)EGifPutWord(Top, GifFile);
    (void)EGifPutWord(Width, GifFile);
    (void)EGifPutWord(Height, GifFile);
    Buf[0] = (ColorMap ? 0x80 : 0x00) |
       (Interlace ? 0x40 : 0x00) |
       (ColorMap ? ColorMap->BitsPerPixel - 1 : 0);
    WRITE(GifFile, Buf, 1);

    /* If we have Global color map - dump it also: */
    if (ColorMap != NULL) {
	int i;
        for (i = 0; i < ColorMap->ColorCount; i++) {
            /* Put the ColorMap out also: */
            Buf[0] = ColorMap->Colors[i].Red;
            Buf[1] = ColorMap->Colors[i].Green;
            Buf[2] = ColorMap->Colors[i].Blue;
            if (WRITE(GifFile, Buf, 3) != 3) {
                _GifError = E_GIF_ERR_WRITE_FAILED;
                return GIF_ERROR;
            }
        }
    }
    if (GifFile->SColorMap == NULL && GifFile->Image.ColorMap == NULL) {
        _GifError = E_GIF_ERR_NO_COLOR_MAP;
        return GIF_ERROR;
    }

    /* Mark this file as has screen descriptor: */
    Private->FileState |= FILE_STATE_IMAGE;
    Private->PixelCount = (long)Width *(long)Height;

    /* Reset compress algorithm parameters. */
    (void)EGifSetupCompress(GifFile);

    return GIF_OK;
}

/******************************************************************************
 * Put one full scanned line (Line) of length LineLen into GIF file.
 *****************************************************************************/
int
EGifPutLine(GifFileType * GifFile, GifPixelType *Line, int LineLen)
{
    int i;
    GifPixelType Mask;
    GifFilePrivateType *Private = (GifFilePrivateType *) GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    if (!LineLen)
        LineLen = GifFile->Image.Width;
    if (Private->PixelCount < (unsigned)LineLen) {
        _GifError = E_GIF_ERR_DATA_TOO_BIG;
        return GIF_ERROR;
    }
    Private->PixelCount -= LineLen;

    /* Make sure the codes are not out of bit range, as we might generate
     * wrong code (because of overflow when we combine them) in this case: */
    Mask = CodeMask[Private->BitsPerPixel];
    for (i = 0; i < LineLen; i++)
        Line[i] &= Mask;

    return EGifCompressLine(GifFile, Line, LineLen);
}

/******************************************************************************
 * Put one pixel (Pixel) into GIF file.
 *****************************************************************************/
int
EGifPutPixel(GifFileType *GifFile, GifPixelType Pixel)
{
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    if (Private->PixelCount == 0) {
        _GifError = E_GIF_ERR_DATA_TOO_BIG;
        return GIF_ERROR;
    }
    --Private->PixelCount;

    /* Make sure the code is not out of bit range, as we might generate
     * wrong code (because of overflow when we combine them) in this case: */
    Pixel &= CodeMask[Private->BitsPerPixel];

    return EGifCompressLine(GifFile, &Pixel, 1);
}

/******************************************************************************
 * Put a comment into GIF file using the GIF89 comment extension block.
 *****************************************************************************/
int
EGifPutComment(GifFileType *GifFile, const char *Comment)
{
    unsigned int length = strlen(Comment);
    char *buf;

    length = strlen(Comment);
    if (length <= 255) {
        return EGifPutExtension(GifFile, COMMENT_EXT_FUNC_CODE,
                                length, Comment);
    } else {
        buf = (char *)Comment;
        if (EGifPutExtensionLeader(GifFile, COMMENT_EXT_FUNC_CODE)
                == GIF_ERROR) {
            return GIF_ERROR;
        }

        /* Break the comment into 255 byte sub blocks */
        while (length > 255) {
            if (EGifPutExtensionBlock(GifFile, 255, buf) == GIF_ERROR) {
                return GIF_ERROR;
            }
            buf = buf + 255;
            length -= 255;
        }
        /* Output any partial block and the clear code. */
        if (length > 0) {
            if (EGifPutExtensionBlock(GifFile, length, buf) == GIF_ERROR) {
                return GIF_ERROR;
            }
        }
	if (EGifPutExtensionTrailer(GifFile) == GIF_ERROR) {
	    return GIF_ERROR;
        }
    }
    return GIF_OK;
}

/******************************************************************************
 * Begin an extension block (see GIF manual).  More
 * extensions can be dumped using EGifPutExtensionBlock until
 * EGifPutExtensionTrailer is invoked.
 *****************************************************************************/
int
EGifPutExtensionLeader(GifFileType *GifFile, const int ExtCode)
{
    GifByteType Buf[3];
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    Buf[0] = EXTENSION_INTRODUCER;
    Buf[1] = ExtCode;
    WRITE(GifFile, Buf, 2);

    return GIF_OK;
}

/******************************************************************************
 * Put extension block data (see GIF manual) into gif file.
 *****************************************************************************/
int
EGifPutExtensionBlock(GifFileType *GifFile, 
		     const int ExtLen,
		     const void *Extension)
{
    GifByteType Buf;
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    Buf = ExtLen;
    WRITE(GifFile, &Buf, 1);
    WRITE(GifFile, Extension, ExtLen);

    return GIF_OK;
}

/******************************************************************************
 * Put a terminating block (see GIF manual) into gif file.
 *****************************************************************************/
int
EGifPutExtensionTrailer(GifFileType *GifFile) {

    GifByteType Buf;
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    /* Write the block terminator */
    Buf = 0;
    WRITE(GifFile, &Buf, 1);

    return GIF_OK;
}

/******************************************************************************
 * Put an extension block (see GIF manual) into gif file.
 * Warning: This function is only useful for Extension blocks that have at
 * most one subblock.  Extensions with more than one subblock need to use the
 * EGifPutExtension{First,Next,Last} functions instead.
 *****************************************************************************/
int
EGifPutExtension(GifFileType *GifFile,
                 const int ExtCode,
                 const int ExtLen,
                 const void *Extension) {

    GifByteType Buf[3];
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    if (ExtCode == 0)
        WRITE(GifFile, (GifByteType *)&ExtLen, 1);
    else {
        Buf[0] = EXTENSION_INTRODUCER;
        Buf[1] = ExtCode;   /* Extension Label */
        Buf[2] = ExtLen;    /* Extension length */
        WRITE(GifFile, Buf, 3);
    }
    WRITE(GifFile, Extension, ExtLen);
    Buf[0] = 0;
    WRITE(GifFile, Buf, 1);

    return GIF_OK;
}

/******************************************************************************
 * Render a Graphics Control Block as raw extension data
 *****************************************************************************/

size_t EGifGCBToExtension(const GraphicsControlBlock *GCB,
		       GifByteType *GifExtension)
{
    GifExtension[0] = 0;
    GifExtension[0] |= (GCB->TransparentColor == NO_TRANSPARENT_COLOR) ? 0x00 : 0x01;
    GifExtension[0] |= GCB->UserInputFlag ? 0x02 : 0x00;
    GifExtension[0] |= ((GCB->DisposalMode & 0x07) << 2);
    GifExtension[1] = GCB->DelayTime & 0xff;
    GifExtension[2] = (GCB->DelayTime >> 8) & 0xff;
    GifExtension[3] = (char)GCB->TransparentColor;
    return 4;
}

/******************************************************************************
 * Replace the Graphics Control Block for a saved image, if it exists.
 *****************************************************************************/

int EGifGCBToSavedExtension(const GraphicsControlBlock *GCB, 
			    GifFileType *GifFile, int ImageIndex)
{
    int i;
    size_t Len;
    GifByteType buf[sizeof(GraphicsControlBlock)]; /* a bit dodgy... */

    if (ImageIndex < 0 || ImageIndex > GifFile->ImageCount - 1)
	return GIF_ERROR;

    for (i = 0; i < GifFile->SavedImages[ImageIndex].Leading.ExtensionBlockCount; i++) {
	ExtensionBlock *ep = &GifFile->SavedImages[ImageIndex].Leading.ExtensionBlocks[i];
	if (ep->Function == GRAPHICS_EXT_FUNC_CODE) {
	    EGifGCBToExtension(GCB, ep->Bytes);
	    return GIF_OK;
	}
    }

    Len = EGifGCBToExtension(GCB, (GifByteType *)buf);
    if (GifAddExtensionBlock(&GifFile->SavedImages[ImageIndex].Leading,
			  GRAPHICS_EXT_FUNC_CODE,
			  Len,
			  (unsigned char *)buf) == GIF_ERROR)
	return (GIF_ERROR);

    return (GIF_OK);
}

/******************************************************************************
 * Put the image code in compressed form. This routine can be called if the
 * information needed to be piped out as is. Obviously this is much faster
 * than decoding and encoding again. This routine should be followed by calls
 * to EGifPutCodeNext, until NULL block is given.
 * The block should NOT be freed by the user (not dynamically allocated).
 *****************************************************************************/
int
EGifPutCode(GifFileType *GifFile, int CodeSize, const GifByteType *CodeBlock)
{
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    /* No need to dump code size as Compression set up does any for us: */
    /* 
     * Buf = CodeSize;
     * if (WRITE(GifFile, &Buf, 1) != 1) {
     *      _GifError = E_GIF_ERR_WRITE_FAILED;
     *      return GIF_ERROR;
     * }
     */

    return EGifPutCodeNext(GifFile, CodeBlock);
}

/******************************************************************************
 * Continue to put the image code in compressed form. This routine should be
 * called with blocks of code as read via DGifGetCode/DGifGetCodeNext. If
 * given buffer pointer is NULL, empty block is written to mark end of code.
 *****************************************************************************/
int
EGifPutCodeNext(GifFileType *GifFile, const GifByteType *CodeBlock)
{
    GifByteType Buf;
    GifFilePrivateType *Private = (GifFilePrivateType *)GifFile->Private;

    if (CodeBlock != NULL) {
        if (WRITE(GifFile, CodeBlock, CodeBlock[0] + 1)
               != (unsigned)(CodeBlock[0] + 1)) {
            _GifError = E_GIF_ERR_WRITE_FAILED;
            return GIF_ERROR;
        }
    } else {
        Buf = 0;
        if (WRITE(GifFile, &Buf, 1) != 1) {
            _GifError = E_GIF_ERR_WRITE_FAILED;
            return GIF_ERROR;
        }
        Private->PixelCount = 0;    /* And local info. indicate image read. */
    }

    return GIF_OK;
}

/******************************************************************************
 * This routine should be called last, to close GIF file.
 *****************************************************************************/
int
EGifCloseFile(GifFileType *GifFile)
{
    GifByteType Buf;
    GifFilePrivateType *Private;
    FILE *File;

    if (GifFile == NULL)
        return GIF_ERROR;

    Private = (GifFilePrivateType *) GifFile->Private;
    if (Private == NULL)
	return GIF_ERROR;
    if (!IS_WRITEABLE(Private)) {
        /* This file was NOT open for writing: */
        _GifError = E_GIF_ERR_NOT_WRITEABLE;
        return GIF_ERROR;
    }

    File = Private->File;

    Buf = TERMINATOR_INTRODUCER;
    WRITE(GifFile, &Buf, 1);

    if (GifFile->Image.ColorMap) {
        GifFreeMapObject(GifFile->Image.ColorMap);
        GifFile->Image.ColorMap = NULL;
    }
    if (GifFile->SColorMap) {
        GifFreeMapObject(GifFile->SColorMap);
        GifFile->SColorMap = NULL;
    }
    if (Private) {
        if (Private->HashTable) {
            free((char *) Private->HashTable);
        }
	    free((char *) Private);
    }
    free(GifFile);

    if (File && fclose(File) != 0) {
        _GifError = E_GIF_ERR_CLOSE_FAILED;
        return GIF_ERROR;
    }
    return GIF_OK;
}

/******************************************************************************
 * Put 2 bytes (word) into the given file:
 *****************************************************************************/
static int
EGifPutWord(int Word, GifFileType *GifFile)
{
    unsigned char c[2];

    c[0] = Word & 0xff;
    c[1] = (Word >> 8) & 0xff;
    if (WRITE(GifFile, c, 2) == 2)
        return GIF_OK;
    else
        return GIF_ERROR;
}

/******************************************************************************
 * Setup the LZ compression for this image:
 *****************************************************************************/
static int
EGifSetupCompress(GifFileType *GifFile)
{
    int BitsPerPixel;
    GifByteType Buf;
    GifFilePrivateType *Private = (GifFilePrivateType *) GifFile->Private;

    /* Test and see what color map to use, and from it # bits per pixel: */
    if (GifFile->Image.ColorMap)
        BitsPerPixel = GifFile->Image.ColorMap->BitsPerPixel;
    else if (GifFile->SColorMap)
        BitsPerPixel = GifFile->SColorMap->BitsPerPixel;
    else {
        _GifError = E_GIF_ERR_NO_COLOR_MAP;
        return GIF_ERROR;
    }

    Buf = BitsPerPixel = (BitsPerPixel < 2 ? 2 : BitsPerPixel);
    WRITE(GifFile, &Buf, 1);    /* Write the Code size to file. */

    Private->Buf[0] = 0;    /* Nothing was output yet. */
    Private->BitsPerPixel = BitsPerPixel;
    Private->ClearCode = (1 << BitsPerPixel);
    Private->EOFCode = Private->ClearCode + 1;
    Private->RunningCode = Private->EOFCode + 1;
    Private->RunningBits = BitsPerPixel + 1;    /* Number of bits per code. */
    Private->MaxCode1 = 1 << Private->RunningBits;    /* Max. code + 1. */
    Private->CrntCode = FIRST_CODE;    /* Signal that this is first one! */
    Private->CrntShiftState = 0;    /* No information in CrntShiftDWord. */
    Private->CrntShiftDWord = 0;

   /* Clear hash table and send Clear to make sure the decoder do the same. */
    _ClearHashTable(Private->HashTable);

    if (EGifCompressOutput(GifFile, Private->ClearCode) == GIF_ERROR) {
        _GifError = E_GIF_ERR_DISK_IS_FULL;
        return GIF_ERROR;
    }
    return GIF_OK;
}

/******************************************************************************
 * The LZ compression routine:
 * This version compresses the given buffer Line of length LineLen.
 * This routine can be called a few times (one per scan line, for example), in
 * order to complete the whole image.
******************************************************************************/
static int
EGifCompressLine(GifFileType *GifFile,
                 GifPixelType *Line,
                 const int LineLen)
{
    int i = 0, CrntCode, NewCode;
    unsigned long NewKey;
    GifPixelType Pixel;
    GifHashTableType *HashTable;
    GifFilePrivateType *Private = (GifFilePrivateType *) GifFile->Private;

    HashTable = Private->HashTable;

    if (Private->CrntCode == FIRST_CODE)    /* Its first time! */
        CrntCode = Line[i++];
    else
        CrntCode = Private->CrntCode;    /* Get last code in compression. */

    while (i < LineLen) {   /* Decode LineLen items. */
        Pixel = Line[i++];  /* Get next pixel from stream. */
        /* Form a new unique key to search hash table for the code combines 
         * CrntCode as Prefix string with Pixel as postfix char.
         */
        NewKey = (((uint32_t) CrntCode) << 8) + Pixel;
        if ((NewCode = _ExistsHashTable(HashTable, NewKey)) >= 0) {
            /* This Key is already there, or the string is old one, so
             * simple take new code as our CrntCode:
             */
            CrntCode = NewCode;
        } else {
            /* Put it in hash table, output the prefix code, and make our
             * CrntCode equal to Pixel.
             */
            if (EGifCompressOutput(GifFile, CrntCode) == GIF_ERROR) {
                _GifError = E_GIF_ERR_DISK_IS_FULL;
                return GIF_ERROR;
            }
            CrntCode = Pixel;

            /* If however the HashTable if full, we send a clear first and
             * Clear the hash table.
             */
            if (Private->RunningCode >= LZ_MAX_CODE) {
                /* Time to do some clearance: */
                if (EGifCompressOutput(GifFile, Private->ClearCode)
                        == GIF_ERROR) {
                    _GifError = E_GIF_ERR_DISK_IS_FULL;
                    return GIF_ERROR;
                }
                Private->RunningCode = Private->EOFCode + 1;
                Private->RunningBits = Private->BitsPerPixel + 1;
                Private->MaxCode1 = 1 << Private->RunningBits;
                _ClearHashTable(HashTable);
            } else {
                /* Put this unique key with its relative Code in hash table: */
                _InsertHashTable(HashTable, NewKey, Private->RunningCode++);
            }
        }

    }

    /* Preserve the current state of the compression algorithm: */
    Private->CrntCode = CrntCode;

    if (Private->PixelCount == 0) {
        /* We are done - output last Code and flush output buffers: */
        if (EGifCompressOutput(GifFile, CrntCode) == GIF_ERROR) {
            _GifError = E_GIF_ERR_DISK_IS_FULL;
            return GIF_ERROR;
        }
        if (EGifCompressOutput(GifFile, Private->EOFCode) == GIF_ERROR) {
            _GifError = E_GIF_ERR_DISK_IS_FULL;
            return GIF_ERROR;
        }
        if (EGifCompressOutput(GifFile, FLUSH_OUTPUT) == GIF_ERROR) {
            _GifError = E_GIF_ERR_DISK_IS_FULL;
            return GIF_ERROR;
        }
    }

    return GIF_OK;
}

/******************************************************************************
 * The LZ compression output routine:
 * This routine is responsible for the compression of the bit stream into
 * 8 bits (bytes) packets.
 * Returns GIF_OK if written succesfully.
 *****************************************************************************/
static int
EGifCompressOutput(GifFileType *GifFile,
                   const int Code)
{
    GifFilePrivateType *Private = (GifFilePrivateType *) GifFile->Private;
    int retval = GIF_OK;

    if (Code == FLUSH_OUTPUT) {
        while (Private->CrntShiftState > 0) {
            /* Get Rid of what is left in DWord, and flush it. */
            if (EGifBufferedOutput(GifFile, Private->Buf,
                                 Private->CrntShiftDWord & 0xff) == GIF_ERROR)
                retval = GIF_ERROR;
            Private->CrntShiftDWord >>= 8;
            Private->CrntShiftState -= 8;
        }
        Private->CrntShiftState = 0;    /* For next time. */
        if (EGifBufferedOutput(GifFile, Private->Buf,
                               FLUSH_OUTPUT) == GIF_ERROR)
            retval = GIF_ERROR;
    } else {
        Private->CrntShiftDWord |= ((long)Code) << Private->CrntShiftState;
        Private->CrntShiftState += Private->RunningBits;
        while (Private->CrntShiftState >= 8) {
            /* Dump out full bytes: */
            if (EGifBufferedOutput(GifFile, Private->Buf,
                                 Private->CrntShiftDWord & 0xff) == GIF_ERROR)
                retval = GIF_ERROR;
            Private->CrntShiftDWord >>= 8;
            Private->CrntShiftState -= 8;
        }
    }

    /* If code cannt fit into RunningBits bits, must raise its size. Note */
    /* however that codes above 4095 are used for special signaling.      */
    if (Private->RunningCode >= Private->MaxCode1 && Code <= 4095) {
       Private->MaxCode1 = 1 << ++Private->RunningBits;
    }

    return retval;
}

/******************************************************************************
 * This routines buffers the given characters until 255 characters are ready
 * to be output. If Code is equal to -1 the buffer is flushed (EOF).
 * The buffer is Dumped with first byte as its size, as GIF format requires.
 * Returns GIF_OK if written succesfully.
 *****************************************************************************/
static int
EGifBufferedOutput(GifFileType *GifFile,
                   GifByteType *Buf,
                   int c)
{
    if (c == FLUSH_OUTPUT) {
        /* Flush everything out. */
        if (Buf[0] != 0
            && WRITE(GifFile, Buf, Buf[0] + 1) != (unsigned)(Buf[0] + 1)) {
            _GifError = E_GIF_ERR_WRITE_FAILED;
            return GIF_ERROR;
        }
        /* Mark end of compressed data, by an empty block (see GIF doc): */
        Buf[0] = 0;
        if (WRITE(GifFile, Buf, 1) != 1) {
            _GifError = E_GIF_ERR_WRITE_FAILED;
            return GIF_ERROR;
        }
    } else {
        if (Buf[0] == 255) {
            /* Dump out this buffer - it is full: */
            if (WRITE(GifFile, Buf, Buf[0] + 1) != (unsigned)(Buf[0] + 1)) {
                _GifError = E_GIF_ERR_WRITE_FAILED;
                return GIF_ERROR;
            }
            Buf[0] = 0;
        }
        Buf[++Buf[0]] = c;
    }

    return GIF_OK;
}

/******************************************************************************
 * This routine writes to disk an in-core representation of a GIF previously
 * created by DGifSlurp().
 *****************************************************************************/

static int
EGifWriteExtensions(GifFileType *GifFileOut, 
			       ExtensionBlock *ExtensionBlocks, 
			       int ExtensionBlockCount) 
{
    if (ExtensionBlocks) {
        ExtensionBlock *ep;
	int j;

	for (j = 0; j < ExtensionBlockCount; j++) {
	    ep = &ExtensionBlocks[j];
	    if (ep->Function != CONTINUE_EXT_FUNC_CODE)
		if (EGifPutExtensionLeader(GifFileOut, ep->Function) == GIF_ERROR)
		    return (GIF_ERROR);
	    if (EGifPutExtensionBlock(GifFileOut, ep->ByteCount, ep->Bytes) == GIF_ERROR)
		return (GIF_ERROR);
	    if (j == ExtensionBlockCount - 1 || (ep+1)->Function != CONTINUE_EXT_FUNC_CODE)
		if (EGifPutExtensionTrailer(GifFileOut) == GIF_ERROR)
		    return (GIF_ERROR);
	}
    }

    return (GIF_OK);
}

int
EGifSpew(GifFileType *GifFileOut) 
{
    int i, j; 

    if (EGifPutScreenDesc(GifFileOut,
                          GifFileOut->SWidth,
                          GifFileOut->SHeight,
                          GifFileOut->SColorResolution,
                          GifFileOut->SBackGroundColor,
                          GifFileOut->SColorMap) == GIF_ERROR) {
        return (GIF_ERROR);
    }

    for (i = 0; i < GifFileOut->ImageCount; i++) {
        SavedImage *sp = &GifFileOut->SavedImages[i];
        int SavedHeight = sp->ImageDesc.Height;
        int SavedWidth = sp->ImageDesc.Width;

        /* this allows us to delete images by nuking their rasters */
        if (sp->RasterBits == NULL)
            continue;

	if (EGifWriteExtensions(GifFileOut, 
				sp->Leading.ExtensionBlocks,
				sp->Leading.ExtensionBlockCount) == GIF_ERROR)
	    return (GIF_ERROR);

        if (EGifPutImageDesc(GifFileOut,
                             sp->ImageDesc.Left,
                             sp->ImageDesc.Top,
                             SavedWidth,
                             SavedHeight,
                             sp->ImageDesc.Interlace,
                             sp->ImageDesc.ColorMap) == GIF_ERROR)
            return (GIF_ERROR);

	if (sp->ImageDesc.Interlace) {
	     /* 
	      * The way an interlaced image should be written - 
	      * offsets and jumps...
	      */
	    int InterlacedOffset[] = { 0, 4, 2, 1 };
	    int InterlacedJumps[] = { 8, 8, 4, 2 };
	    int k;
	    /* Need to perform 4 passes on the images: */
	    for (k = 0; k < 4; k++)
		for (j = InterlacedOffset[k]; 
		     j < SavedHeight;
		     j += InterlacedJumps[k]) {
		    if (EGifPutLine(GifFileOut, 
				    sp->RasterBits + j * SavedWidth, 
				    SavedWidth)	== GIF_ERROR)
			return (GIF_ERROR);
		}
	} else {
	    for (j = 0; j < SavedHeight; j++) {
		if (EGifPutLine(GifFileOut,
				sp->RasterBits + j * SavedWidth,
				SavedWidth) == GIF_ERROR)
		    return (GIF_ERROR);
	    }
	}
    }

    if (EGifWriteExtensions(GifFileOut,
			    GifFileOut->Trailing.ExtensionBlocks,
			    GifFileOut->Trailing.ExtensionBlockCount) == GIF_ERROR)
	return (GIF_ERROR);

    if (EGifCloseFile(GifFileOut) == GIF_ERROR)
        return (GIF_ERROR);

    return (GIF_OK);
}

/* end */
