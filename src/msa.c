/*
  Hatari

  MSA Disc support
*/

#include "main.h"
#include "file.h"
#include "floppy.h"
#include "memAlloc.h"
#include "misc.h"
#include "stMemory.h"

#define SAVE_TO_MSA_IMAGES

/*
    .MSA FILE FORMAT
  --================------------------------------------------------------------

  For those interested, an MSA file is made up as follows:

  Header:

  Word  ID marker, should be $0E0F
  Word  Sectors per track
  Word  Sides (0 or 1; add 1 to this to get correct number of sides)
  Word  Starting track (0-based)
  Word  Ending track (0-based)

  Individual tracks follow the header in alternating side order, e.g. a double
  sided disk is stored as:

  TRACK 0, SIDE 0
  TRACK 0, SIDE 1
  TRACK 1, SIDE 0
  TRACK 1, SIDE 1
  TRACK 2, SIDE 0
  TRACK 2, SIDE 1

  ...and so on. Track blocks are made up as follows:

  Word  Data length
  Bytes  Data

  If the data length is equal to 512 x the sectors per track value, it is an
  uncompressed track and you can merely copy the data to the appropriate track
  of the disk. However, if the data length value is less than 512 x the sectors
  per track value it is a compressed track.

  Compressed tracks use simple a Run Length Encoding (RLE) compression method.
  You can directly copy any data bytes until you find an $E5 byte. This signals
  a compressed run, and is made up as follows:

  Byte  Marker - $E5
  Byte  Data byte
  Word  Run length

  So, if MSA found six $AA bytes in a row it would encode it as:

  $E5AA0006

  What happens if there's an actual $E5 byte on the disk? Well, logically
  enough, it is encoded as:

  $E5E50001

  This is obviously bad news if a disk consists of lots of data like
  $E500E500E500E500... but if MSA makes a track bigger when attempting to
  compress it, it just stores the uncompressed version instead.

  MSA only compresses runs of at least 4 identical bytes (after all, it would be
  wasteful to store 4 bytes for a run of only 3 identical bytes!). There is one
  exception to this rule: if a run of 2 or 3 $E5 bytes is found, that is stored
  appropriately enough as a run. Again, it would be wasteful to store 4 bytes
  for every single $E5 byte.

  The hacked release of MSA that enables the user to turn off compression
  completely simply stops MSA from trying this compression and produces MSA
  images that are completely uncompressed. This is okay because it is possible
  for MSA to produce such an image anyway, and such images are therefore 100%
  compatible with normal MSA versions (and MSA-to-ST of course).
*/

typedef struct {
  short int ID;                 /* Word  ID marker, should be $0E0F */
  short int SectorsPerTrack;    /* Word  Sectors per track */
  short int Sides;              /* Word  Sides (0 or 1; add 1 to this to get correct number of sides) */
  short int StartingTrack;      /* Word  Starting track (0-based) */
  short int EndingTrack;        /* Word  Ending track (0-based) */
} MSAHEADERSTRUCT;

#define MSA_WORKSPACE_SIZE  (1024*1024)  /* Size of workspace to use when saving MSA files */


//-----------------------------------------------------------------------
/*
  Uncompress .MSA data into buffer
*/
int MSA_UnCompress(unsigned char *pMSAFile,unsigned char *pBuffer)
{
  MSAHEADERSTRUCT *pMSAHeader;
  unsigned char *pMSAImageBuffer,*pImageBuffer;
  unsigned char Byte,Data;
  int ImageSize = 0;
  int i,Track,Side,DataLength,NumBytesUnCompressed,RunLength;

  // Is an '.msa' file?? Check header
  pMSAHeader = (MSAHEADERSTRUCT *)pMSAFile;
  if (pMSAHeader->ID==STMemory_Swap68000Int(0x0E0F)) {
    // First swap 'header' words around to PC format - easier later on
    pMSAHeader->SectorsPerTrack = STMemory_Swap68000Int(pMSAHeader->SectorsPerTrack);
    pMSAHeader->Sides = STMemory_Swap68000Int(pMSAHeader->Sides);
    pMSAHeader->StartingTrack = STMemory_Swap68000Int(pMSAHeader->StartingTrack);
    pMSAHeader->EndingTrack = STMemory_Swap68000Int(pMSAHeader->EndingTrack);

    // Set pointers
    pImageBuffer = (unsigned char *)pBuffer;
    pMSAImageBuffer = (unsigned char *)((unsigned long)pMSAFile+sizeof(MSAHEADERSTRUCT));

    // Uncompress to memory as '.ST' disc image - NOTE assumes 512 bytes per sector(use NUMBYTESPERSECTOR define)!!!
    for(Track=pMSAHeader->StartingTrack; Track<(pMSAHeader->EndingTrack+1); Track++) {
      for(Side=0; Side<(pMSAHeader->Sides+1); Side++) {
        // Uncompress MSA Track, first check if is not compressed
        DataLength = STMemory_Swap68000Int(*(short int *)pMSAImageBuffer);
        pMSAImageBuffer += sizeof(short int);
        if (DataLength==(NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack)) {
          // No compression on track, simply copy and continue
          memcpy(pImageBuffer,pMSAImageBuffer,NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack);
          pImageBuffer += NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack;
          pMSAImageBuffer += DataLength;
        }
        else {
          // Uncompress track
          NumBytesUnCompressed = 0;
          while(NumBytesUnCompressed<(NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack)) {
            Byte = *pMSAImageBuffer++;
            if (Byte!=0xE5) {                           // Compressed header??
              *pImageBuffer++ = Byte;                   // No, just copy byte
              NumBytesUnCompressed++;
            }
            else {
              Data = *pMSAImageBuffer++;                // Byte to copy
              RunLength = STMemory_Swap68000Int(*(short int *)pMSAImageBuffer);  // For length
              // Limit length to size of track, incorrect images may overflow
              if ( (RunLength+NumBytesUnCompressed)>(NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack) )
                RunLength = (NUMBYTESPERSECTOR*pMSAHeader->SectorsPerTrack)-NumBytesUnCompressed;
              pMSAImageBuffer += sizeof(short int);
              for(i=0; i<RunLength; i++)
                *pImageBuffer++ = Data;                 // Copy byte
              NumBytesUnCompressed += RunLength;
            }
          }
        }          
      }
    }

    // Set size of loaded image
    ImageSize = (unsigned long)pImageBuffer-(unsigned long)pBuffer;
  }

  // Return number of bytes loaded, '0' if failed
  return(ImageSize);
}

//-----------------------------------------------------------------------
/*
  Uncompress .MSA file into memory, return number of bytes loaded
*/
int MSA_ReadDisc(char *pszFileName,unsigned char *pBuffer)
{
  unsigned char *pMSAFile;
  int ImageSize = 0;

  // Read in file
  pMSAFile = (unsigned char *)File_Read(pszFileName,NULL,NULL,NULL);
  if (pMSAFile) {
    // Uncompress into disc buffer
    ImageSize = MSA_UnCompress(pMSAFile,pBuffer);

    // Free MSA file we loaded
    Memory_Free(pMSAFile);
  }

  // Return number of bytes loaded, '0' if failed
  return(ImageSize);
}

//-----------------------------------------------------------------------
/*
  Return number of bytes of the same byte in the passed buffer
  If we return '0' this means no run(or end of buffer)
*/
int MSA_FindRunOfBytes(unsigned char *pBuffer,int nBytesInBuffer)
{
  unsigned char ScannedByte;
  int nTotalRun;
  int i;

  // Do we enough for a run?
  if (nBytesInBuffer<2)
    return(0);

  // OK, scan for run
  nTotalRun = 1;
  ScannedByte = *pBuffer++;
  // Is this the marker? If so, this is a run of one
  if (ScannedByte!=0xE5) {
    for(i=1; i<nBytesInBuffer; i++) {
      if (*pBuffer++==ScannedByte)
        nTotalRun++;
      else
        break;
    }
    // Was this enough of a run to make a difference?
    if (nTotalRun<4)
      nTotalRun = 0;    // Just store a bytes
  }

  return(nTotalRun);
}

//-----------------------------------------------------------------------
/*
  Save compressed .MSA file from memory buffer. Returns TRUE is all OK
*/
BOOL MSA_WriteDisc(char *pszFileName,unsigned char *pBuffer,int ImageSize)
{
#ifdef SAVE_TO_MSA_IMAGES

  MSAHEADERSTRUCT *pMSAHeader;
  unsigned short int *pMSADataLength;
  unsigned char *pMSAImageBuffer, *pMSABuffer, *pImageBuffer;
  unsigned short int nSectorsPerTrack, nSides, nCompressedBytes, nBytesPerTrack;
  BOOL nRet;
  int nTracks,nBytesToGo,nBytesRun;
  int Track,Side;

  // Allocate workspace for compressed image
  pMSAImageBuffer = (unsigned char *)Memory_Alloc(MSA_WORKSPACE_SIZE);

  // Store header
  pMSAHeader = (MSAHEADERSTRUCT *)pMSAImageBuffer;
  pMSAHeader->ID = STMemory_Swap68000Int(0x0E0F);
  Floppy_FindDiscDetails(pBuffer,ImageSize,&nSectorsPerTrack,&nSides);
  pMSAHeader->SectorsPerTrack = STMemory_Swap68000Int(nSectorsPerTrack);
  pMSAHeader->Sides = STMemory_Swap68000Int(nSides-1);
  pMSAHeader->StartingTrack = STMemory_Swap68000Int(0);
  nTracks = ((ImageSize / NUMBYTESPERSECTOR) / nSectorsPerTrack) / nSides;
  pMSAHeader->EndingTrack = STMemory_Swap68000Int(nTracks-1);

  // Compress image
  pMSABuffer = pMSAImageBuffer + sizeof(MSAHEADERSTRUCT);
  for(Track=0; Track<nTracks; Track++) {
    for(Side=0; Side<nSides; Side++) {
      // Get track data pointer
      nBytesPerTrack = NUMBYTESPERSECTOR*nSectorsPerTrack;
      pImageBuffer = pBuffer + (nBytesPerTrack*Side) + ((nBytesPerTrack*nSides)*Track);

      // Skip data length(fill in later)
      pMSADataLength = (unsigned short int *)pMSABuffer;
      pMSABuffer += sizeof(short int);
      
      // Compress track
      nBytesToGo = NUMBYTESPERSECTOR * nSectorsPerTrack;
      nCompressedBytes = 0;
      while(nBytesToGo>0) {
        nBytesRun = MSA_FindRunOfBytes(pImageBuffer,nBytesToGo);
        if (nBytesRun==0) {
          // Just copy byte
          *pMSABuffer++ = *pImageBuffer++;
          nCompressedBytes++;
          nBytesRun = 1;
        }
        else {
          // Store run!
          *pMSABuffer++ = 0xE5;               // Marker
          *pMSABuffer++ = *pImageBuffer;      // Byte, and follow with 16-bit length
          *(short int *)pMSABuffer = STMemory_Swap68000Int(nBytesRun);
          pMSABuffer += sizeof(short int);
          pImageBuffer += nBytesRun;
          nCompressedBytes += 4;
        }
        nBytesToGo -= nBytesRun;
      }

      // Is compressed track smaller than the original?
      if (nCompressedBytes<(NUMBYTESPERSECTOR*nSectorsPerTrack)) {
        // Yes, store size
        *pMSADataLength = STMemory_Swap68000Int(nCompressedBytes);
      }
      else {
        // No, just store uncompressed track
        *pMSADataLength++ = STMemory_Swap68000Int(NUMBYTESPERSECTOR*nSectorsPerTrack);
        pMSABuffer = (unsigned char *)pMSADataLength;
        pImageBuffer = pBuffer + (nBytesPerTrack*Side) + ((nBytesPerTrack*nSides)*Track);
        memcpy(pMSABuffer,pImageBuffer,(NUMBYTESPERSECTOR*nSectorsPerTrack));
        pMSABuffer += (NUMBYTESPERSECTOR*nSectorsPerTrack);
      }
    }
  }

  // And save to file!
  nRet = File_Save(/*hWnd,*/pszFileName,pMSAImageBuffer,pMSABuffer-pMSAImageBuffer,FALSE);

  // Free workspace
  Memory_Free(pMSAImageBuffer);

  return(nRet);

#else  //SAVE_TO_MSA_IMAGES

  // Oops, cannot save
  return(FALSE);

#endif  //SAVE_TO_MSA_IMAGES
}
