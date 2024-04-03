#include <memory.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <fcntl.h>
#include <iostream>
#include <stdio.h>
#include "page.h"
#include "buf.h"

#define ASSERT(c)  { if (!(c)) { \
		       cerr << "At line " << __LINE__ << ":" << endl << "  "; \
                       cerr << "This condition should hold: " #c << endl; \
                       exit(1); \
		     } \
                   }

//----------------------------------------
// Constructor of the class BufMgr
//----------------------------------------

BufMgr::BufMgr(const int bufs)
{
    numBufs = bufs;

    bufTable = new BufDesc[bufs];
    memset(bufTable, 0, bufs * sizeof(BufDesc));
    for (int i = 0; i < bufs; i++) 
    {
        bufTable[i].frameNo = i;
        bufTable[i].valid = false;
    }

    bufPool = new Page[bufs];
    memset(bufPool, 0, bufs * sizeof(Page));

    int htsize = ((((int) (bufs * 1.2))*2)/2)+1;
    hashTable = new BufHashTbl (htsize);  // allocate the buffer hash table

    clockHand = bufs - 1;
}


BufMgr::~BufMgr() {

    // flush out all unwritten pages
    for (int i = 0; i < numBufs; i++) 
    {
        BufDesc* tmpbuf = &bufTable[i];
        if (tmpbuf->valid == true && tmpbuf->dirty == true) {

#ifdef DEBUGBUF
            cout << "flushing page " << tmpbuf->pageNo
                 << " from frame " << i << endl;
#endif

            tmpbuf->file->writePage(tmpbuf->pageNo, &(bufPool[i]));
        }
    }

    delete [] bufTable;
    delete [] bufPool;
}

/* Function responsible for allocating a free frame using the clock algorithm.
 * If the frame has been written to in the buffer, this function is also responsible
 * for writing the changed page back to disk. 
 * 
 * INPUTS:
 *    -   int & frame: object used to assign the found free frame
 * 
 * OUTPUTS:
 *    -   Status: enumeration representing the status of the allocation process. Possible values:
 *           -   OK: Successful reading of page
 *           -   UNIXERR: Error occurred while a dirty page was begin written to disk
 *           -   BUFFEREXCEEDED: all buffer frames currently pinned (no buffer frame available)
 */
const Status BufMgr::allocBuf(int & frame) 
{
    // keep track of how many frames we have visited
    int framesVisited = 0;

    // true when we can have a frame we can use
    bool setFrame = false;

    // default return value
    Status retVal = OK;

    while(framesVisited < numBufs*2 && !setFrame){
        //move clockhand to next frame
        advanceClock();

        framesVisited++;

        //check valid set
        if(bufTable[clockHand].valid == false){
            // invoke set on frame
            setFrame = true;
        }
        else{
            //there was a valid set, check refbit
            if(bufTable[clockHand].refbit){
                bufStats.accesses++;

                //clear refbit
                bufTable[clockHand].refbit = false;
            }
            //not a refbit
            else{
                //check if page pinned
                if(bufTable[clockHand].pinCnt == 0){
                    //page is not pinned
                    //remove from hashTable
                    hashTable->remove(bufTable[clockHand].file, bufTable[clockHand].pageNo);

                    //invoke set on frame
                    setFrame = true;

                }
            }
        }

    }

    //check if buffer pool is full
    if(framesVisited >= numBufs*2 && setFrame == false){
        return BUFFEREXCEEDED;
    }

    //available frame found, set frame
    if(bufTable[clockHand].dirty){
        bufStats.diskwrites++;

        //write the page changes back
        Status insertReturn = bufTable[clockHand].file->writePage(bufTable[clockHand].pageNo,
                                                     &bufPool[clockHand]);

        if(insertReturn != OK){
            return insertReturn; 
        }
    }

    //set available frame
    frame = clockHand;

    return retVal; 

}

/* Function responsible for reading a specific page from the buffer pool.
 * If the page does not already exist in the pool, then the function handles
 * the reading and copying from disk as well as the updating of any necessary
 * data structures to manage the allocated page in the buffer manager.
 * 
 * INPUTS:
 *    -   File* file: pointer to the file object from which the desired page will be read.
 *    -   int PageNo: page number of the page to be read
 *    -   Page*&: Reference to a pointer to a page object; upon successful execution, will point
 *                to the buffer frame containing the requested page.
 * 
 * OUTPUTS:
 *    -   Status: enumeration representing the status of the allocation process. Possible values:
 *           -   OK: Successful reading of page
 *           -   UNIXERR: Error occurred at Unix level during the reading process
 *           -   BUFFEREXCEEDED: all buffer frames currently pinned (no buffer frame available)
 *           -   HASHTBLERROR: error occurred while inserting or looking up an entry in hash table
 */
const Status BufMgr::readPage(File* file, const int PageNo, Page*& page)
{
    // Check if page in buffer pool and handle both posible cases
    int frameNo;

    //check for error status
    Status hashFound = hashTable->lookup(file, PageNo, frameNo);

    // Case 1: Page not in buffer pool
    if(hashFound == HASHNOTFOUND) {
        // Allocate buffer frame for new page in buffer pool
        Status status = allocBuf(frameNo);
        if(status != OK) { // Check allocation and return error if present
            return status;
        }

        // Read page from disk into newly allocated buffer pool frame
        status = file->readPage(PageNo, &bufPool[frameNo]);
        if(status != OK){
            return status; 
        }

        // Insert entry into hash table
        status = hashTable->insert(file, PageNo, frameNo);
        if(status != OK) { // Check insertion and handle error if present
            // Release allocated buffer frame
            bufTable[frameNo].Clear();
            // Return error status
            return status;
        }

        // Invoke Set() to set up frame
        bufTable[frameNo].Set(file, PageNo);

        // Set page pointer to the allocated buffer frame for the page
        page = &bufPool[frameNo];
    }

    // Case 2: Page in buffer pool
    else {
        bufTable[frameNo].refbit = true; // Set reference bit for page
        bufTable[frameNo].pinCnt++; // Increment pin count for page

        // Set page pointer to the allocated buffer frame for the page
        page = &bufPool[frameNo];
    }

    return OK;
}


const Status BufMgr::unPinPage(File* file, const int PageNo, 
			       const bool dirty) 
{






}

/* Function responsible for allocating an empty page in a specified file,
 * then inserting the page into the buffer pool and setting any necessary
 * data structures to manage the allocated page in the buffer manager.
 * 
 * INPUTS:
 *    -   File* file: pointer to the file object in which the empty page will be allocated.
 *    -   int& pageNo: reference to integer variable where the page number of newly allocated
 *                     page will be stored.
 *    -   Page*&: Reference to a pointer to a page object; upon successful execution, will point
 *                to the allocated buffer frame for the page.
 * 
 * OUTPUTS:
 *    -   Status: enumeration representing the status of the allocation process. Possible values:
 *           -   OK: Successful allocation
 *           -   UNIXERR: Error occurred at Unix level during allocation
 *           -   BUFFEREXCEEDED: all buffer frames currently pinned (no buffer frame available)
 *           -   HASHTBLERROR: error occurred while inserting entry in hash table
 */
const Status BufMgr::allocPage(File* file, int& pageNo, Page*& page) 
{
    // Allocate empty page in specified file
    Status status = file->allocatePage(pageNo);
    if(status != OK) { // Check allocation and return error if present
        return status;
    }

    // Allocate a buffer pool frame for page
    int frameNo;
    status = allocBuf(frameNo);
    if(status != OK) { // Check allocation and return error if present
        return status;
    }

    // Insert entry into hash table
    status = hashTable->insert(file, pageNo, frameNo);
    if(status != OK) { // Check insertion and handle error if present
        // Release allocated buffer frame
        bufTable[frameNo].Clear();
        // Return error status
        return status;
    }

    // Invoke Set() to set up frame
    bufTable[frameNo].Set(file, pageNo);

    // Set page pointer to the allocated buffer frame for the page
    page = &bufPool[frameNo];

    return OK;
}

const Status BufMgr::disposePage(File* file, const int pageNo) 
{
    // see if it is in the buffer pool
    Status status = OK;
    int frameNo = 0;
    status = hashTable->lookup(file, pageNo, frameNo);
    if (status == OK)
    {
        // clear the page
        bufTable[frameNo].Clear();
    }
    status = hashTable->remove(file, pageNo);

    // deallocate it in the file
    return file->disposePage(pageNo);
}

const Status BufMgr::flushFile(const File* file) 
{
  Status status;

  for (int i = 0; i < numBufs; i++) {
    BufDesc* tmpbuf = &(bufTable[i]);
    if (tmpbuf->valid == true && tmpbuf->file == file) {

      if (tmpbuf->pinCnt > 0)
	  return PAGEPINNED;

      if (tmpbuf->dirty == true) {
#ifdef DEBUGBUF
	cout << "flushing page " << tmpbuf->pageNo
             << " from frame " << i << endl;
#endif
	if ((status = tmpbuf->file->writePage(tmpbuf->pageNo,
					      &(bufPool[i]))) != OK)
	  return status;

	tmpbuf->dirty = false;
      }

      hashTable->remove(file,tmpbuf->pageNo);

      tmpbuf->file = NULL;
      tmpbuf->pageNo = -1;
      tmpbuf->valid = false;
    }

    else if (tmpbuf->valid == false && tmpbuf->file == file)
      return BADBUFFER;
  }
  
  return OK;
}


void BufMgr::printSelf(void) 
{
    BufDesc* tmpbuf;
  
    cout << endl << "Print buffer...\n";
    for (int i=0; i<numBufs; i++) {
        tmpbuf = &(bufTable[i]);
        cout << i << "\t" << (char*)(&bufPool[i]) 
             << "\tpinCnt: " << tmpbuf->pinCnt;
    
        if (tmpbuf->valid == true)
            cout << "\tvalid\n";
        cout << endl;
    };
}


