// Copyright (c) 2006 Martin Knafve / hMailServer.com.  
// http://www.hmailserver.com

#include "stdafx.h"

#include "IMAPFolders.h"
#include "IMAPFolder.h"

#include "../Util/Time.h"
#include "../../IMAP/IMAPConfiguration.h"
#include <unordered_map>

#ifdef _DEBUG
#define DEBUG_NEW new(_NORMAL_BLOCK, __FILE__, __LINE__)
#define new DEBUG_NEW
#endif

namespace HM
{


   IMAPFolders::IMAPFolders(__int64 iAccountID, __int64 iParentFolderID) :
      account_id_(iAccountID),
      parent_folder_id_(iParentFolderID)
   {

   }

   IMAPFolders::IMAPFolders() :
      account_id_(0),
      parent_folder_id_(0)
   {

   }


   IMAPFolders::~IMAPFolders()
   {
  
   }

   void IMAPFolders::Refresh()
   {
       boost::lock_guard<boost::recursive_mutex> guard(_mutex);
   
       vecObjects.clear();
   
       SQLCommand command("SELECT folderid, folderparentid, foldername, folderissubscribed, "
                          "foldercurrentuid, foldercreationtime FROM hm_imapfolders "
                          "WHERE folderaccountid = @FOLDERACCOUNTID ORDER BY folderid ASC");
   
       command.AddParameter("@FOLDERACCOUNTID", account_id_);
   
       std::shared_ptr<DALRecordset> pRS = Application::Instance()->GetDBManager()->OpenRecordset(command);
       if (!pRS)
           return;
   
       // Map to store folder ID to folder object
       std::unordered_map<__int64, std::shared_ptr<IMAPFolder>> folderMap;
   
       // First pass: Create folder objects and store them in the map
       while (!pRS->IsEOF())
       {
           __int64 folderID = pRS->GetLongValue("folderid");
           __int64 parentID = pRS->GetLongValue("folderparentid");
           String folderName = pRS->GetStringValue("foldername");
           bool isSubscribed = pRS->GetLongValue("folderissubscribed") == 1;
           unsigned int currentUID = static_cast<unsigned int>(pRS->GetInt64Value("foldercurrentuid"));
           DateTime creationTime = Time::GetDateFromSystemDate(pRS->GetStringValue("foldercreationtime"));
   
           auto folder = std::make_shared<IMAPFolder>(account_id_, parentID);
           folder->SetID(folderID);
           folder->SetFolderName(folderName);
           folder->SetIsSubscribed(isSubscribed);
           folder->SetCurrentUID(currentUID);
           folder->SetCreationTime(creationTime);
   
           folderMap[folderID] = folder;
   
           pRS->MoveNext();
       }
   
       // Second pass: Build the folder hierarchy
       for (const auto& it : folderMap)
       {
          auto &folderID = it.first;
          auto& folder = it.second;
           __int64 parentID = folder->GetParentFolderID();
   
           if (parentID == -1)
           {
               // Root folder
               vecObjects.push_back(folder);
           }
           else
           {
               auto parentIt = folderMap.find(parentID);
               if (parentIt != folderMap.end())
               {
                   // Add to parent's subfolders
                   parentIt->second->GetSubFolders()->AddItem(folder);
               }
               else
               {
                   // Parent not found, handle error
                   String errorMessage;
                   errorMessage.Format(_T("Parent folder with ID %I64d not found for folder ID %I64d"), parentID, folderID);
                   ErrorManager::Instance()->ReportError(ErrorManager::Medium, 5125, "IMAPFolders::Refresh()", errorMessage);
               }
           }
       }
   }

   std::shared_ptr<IMAPFolder> 
   IMAPFolders::GetFolderByName(const String &sName, bool bRecursive)
   { 
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      for(std::shared_ptr<IMAPFolder> pFolder : vecObjects)
      {
         if (pFolder->GetFolderName().Equals(sName, false))
            return pFolder;

         if (bRecursive)
         {
            // Visit this folder.
            std::shared_ptr<IMAPFolders> pSubFolders = pFolder->GetSubFolders();
            pFolder = pSubFolders->GetFolderByName(sName, bRecursive);

            if (pFolder)
               return pFolder;
         }
      }

      std::shared_ptr<IMAPFolder> pEmpty;
      return pEmpty;
   }


  


   std::shared_ptr<IMAPFolder>
   IMAPFolders::GetFolderByFullPath(const String &sPath)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      String hierarchyDelimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      std::vector<String> sVecPath = StringParser::SplitString(sPath, hierarchyDelimiter);

      return GetFolderByFullPath(sVecPath);
   }

   std::shared_ptr<IMAPFolder>
   IMAPFolders::GetFolderByFullPath(const std::vector<String> &vecFolders)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      std::shared_ptr<IMAPFolder> pCurFolder;
      size_t lNoOfParts= vecFolders.size();
      for (unsigned int i = 0; i < lNoOfParts; i++)
      {
         if (pCurFolder)
         {
            String sFoldName = vecFolders[i];
            pCurFolder = pCurFolder->GetSubFolders()->GetFolderByName(sFoldName);
         }
         else
         {
            String sFoldName = vecFolders[i];
            pCurFolder = GetFolderByName(sFoldName);

            if (!pCurFolder)
               return pCurFolder;
         }
      }

      return pCurFolder;
      
   }

   void
   IMAPFolders::RemoveFolder(std::shared_ptr<IMAPFolder> pFolderToRemove)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      auto iterCurPos = vecObjects.begin();
      auto iterEnd = vecObjects.end();

      __int64 lRemoveFolderID = pFolderToRemove->GetID();
      for (; iterCurPos!= iterEnd; iterCurPos++)
      {
         std::shared_ptr<IMAPFolder> pFolder = (*iterCurPos);

         if (pFolder->GetID() == lRemoveFolderID)
         {
            // Remove this folder fro the collection.
            vecObjects.erase(iterCurPos);
            return;
         }
      }
   }
  
   void
   IMAPFolders::CreatePath(std::shared_ptr<IMAPFolders> pParentContainer,
                           const std::vector<String> &vecFolderPath, 
                           bool bAutoSubscribe)
   {
      boost::lock_guard<boost::recursive_mutex> guard(_mutex);

      String hierarchyDelimiter = Configuration::Instance()->GetIMAPConfiguration()->GetHierarchyDelimiter();

      LOG_DEBUG("Creating IMAP folder " + StringParser::JoinVector(vecFolderPath, hierarchyDelimiter));

      std::vector<String> vecTempPath = vecFolderPath;

      std::shared_ptr<IMAPFolder> pParentFolder;

      while (vecTempPath.size() > 0)
      {
         // Get first level.
         String sTopLevel = vecTempPath[0];

         std::shared_ptr<IMAPFolder> pParentCheck = pParentContainer->GetFolderByName(sTopLevel, false);

         if (pParentCheck)
         {
            // This folder already exists. Create next level.
            pParentContainer = pParentCheck->GetSubFolders();
            pParentFolder = pParentCheck;
            vecTempPath = StringParser::GetAllButFirst(vecTempPath);

            continue;
         }

         __int64 iParentFolderID = -1;
         if (pParentFolder)
            iParentFolderID = pParentFolder->GetID();

         std::shared_ptr<IMAPFolder> pFolder = std::shared_ptr<IMAPFolder>(new IMAPFolder(account_id_, iParentFolderID));
         pFolder->SetFolderName(sTopLevel);
         pFolder->SetIsSubscribed(bAutoSubscribe);

         PersistentIMAPFolder::SaveObject(pFolder);

         // Add the folder to the collection.
         pParentContainer->AddItem(pFolder);

         // Go down one folder.
         pParentContainer = pFolder->GetSubFolders();
         
         vecTempPath = StringParser::GetAllButFirst(vecTempPath);
         pParentFolder = pFolder;

      }
   }

   bool
   IMAPFolders::PreSaveObject(std::shared_ptr<IMAPFolder> pObject, XNode *node)
   {
      pObject->SetAccountID(GetAccountID());
      pObject->SetParentFolderID(parent_folder_id_);
      return true;
   }

   std::shared_ptr<IMAPFolder>
   IMAPFolders::GetItemByDBIDRecursive(__int64 folderID)
   {
       boost::lock_guard<boost::recursive_mutex> guard(_mutex);
   
       for (const auto& folder : vecObjects)
       {
           if (folder->GetID() == folderID)
               return folder;
   
           auto foundFolder = folder->GetSubFolders()->GetItemByDBIDRecursive(folderID);
           if (foundFolder)
               return foundFolder;
       }
   
      std::shared_ptr<IMAPFolder> pEmpty;
      return pEmpty;
   }

   __int64 
   IMAPFolders::GetParentID()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns the ID of the IMAP folder in which these folder exists. If this is
   // a top level collection, -1 is returned.
   //---------------------------------------------------------------------------()
   {
      return parent_folder_id_; 
   }

   __int64 
   IMAPFolders::GetAccountID()
   //---------------------------------------------------------------------------()
   // DESCRIPTION:
   // Returns the ID of the account in which these folders exist
   //---------------------------------------------------------------------------()
   {
      return account_id_; 
   }

   String 
   IMAPFolders::GetCollectionName() const 
   {
      if (GetIsPublicFolders_())
         return "PublicFolders"; 
      else
         return "Folders"; 
   }

   bool 
   IMAPFolders::GetIsPublicFolders_() const
   {
      if (account_id_ == 0)
         return true;
      else
         return false;
   }
}

