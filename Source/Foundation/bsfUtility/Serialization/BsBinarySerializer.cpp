//************************************ bs::framework - Copyright 2018 Marko Pintera **************************************//
//*********** Licensed under the MIT license. See LICENSE.md for full terms. This notice is not to be removed. ***********//
#include "Serialization/BsBinarySerializer.h"

#include "Error/BsException.h"
#include "Debug/BsDebug.h"
#include "Reflection/BsIReflectable.h"
#include "Reflection/BsRTTIType.h"
#include "Reflection/BsRTTIField.h"
#include "Reflection/BsRTTIPlainField.h"
#include "Reflection/BsRTTIReflectableField.h"
#include "Reflection/BsRTTIReflectablePtrField.h"
#include "Reflection/BsRTTIManagedDataBlockField.h"
#include "FileSystem/BsDataStream.h"
#include "Utility/BsBufferedBitstream.h"

namespace bs
{
	constexpr UINT32 BinarySerializer::REPORT_AFTER_BYTES;
	constexpr UINT32 BinarySerializer::WRITE_BUFFER_SIZE;
	constexpr UINT32 BinarySerializer::FLUSH_AFTER_BYTES;
	constexpr UINT32 BinarySerializer::PRELOAD_CHUNK_BYTES;

	BinarySerializer::BinarySerializer()
		:mAlloc(&gFrameAlloc())
	{ }

	void BinarySerializer::encode(IReflectable* object, const SPtr<DataStream>& stream, BinarySerializerFlags flags, SerializationContext* context)
	{
		mObjectsToEncode.clear();
		mObjectAddrToId.clear();
		mLastUsedObjectId = 1;
		mContext = context;
		mBuffer.seek(0);

		mAlloc->markFrame();

		BufferedBitstreamWriter bufferedStream(&mBuffer, stream, WRITE_BUFFER_SIZE, FLUSH_AFTER_BYTES);

		Vector<SPtr<IReflectable>> encodedObjects;
		UINT32 objectId = findOrCreatePersistentId(object);
		
		// Encode primary object and its value types
		if (!encodeEntry(object, objectId, bufferedStream, flags))
		{
			BS_LOG(Error, Serialization, "Destination buffer is null or not large enough.");
			return;
		}

		// Encode pointed to objects and their value types
		UnorderedSet<UINT32> serializedObjects;
		while(true)
		{
			auto iter = mObjectsToEncode.begin();
			bool foundObjectToProcess = false;
			for(; iter != mObjectsToEncode.end(); ++iter)
			{
				auto foundExisting = serializedObjects.find(iter->objectId);
				if(foundExisting != serializedObjects.end())
					continue; // Already processed

				SPtr<IReflectable> curObject = iter->object;
				UINT32 curObjectid = iter->objectId;
				serializedObjects.insert(curObjectid);
				mObjectsToEncode.erase(iter);

				if(!encodeEntry(curObject.get(), curObjectid, bufferedStream, flags))
				{
					BS_LOG(Error, Serialization, "Destination buffer is null or not large enough.");
					return;
				}

				foundObjectToProcess = true;

				// Ensure we keep a reference to the object so it isn't released.
				// The system assigns unique IDs to IReflectable objects based on pointer
				// addresses but if objects get released then same address could be assigned twice.
				// Note: To get around this I could assign unique IDs to IReflectable objects
				encodedObjects.push_back(curObject);

				break; // Need to start over as mObjectsToSerialize was possibly modified
			}

			if(!foundObjectToProcess) // We're done
				break;
		}

		bufferedStream.flush(true);

		encodedObjects.clear();
		mObjectsToEncode.clear();
		mObjectAddrToId.clear();

		mAlloc->clear();
	}

	SPtr<IReflectable> BinarySerializer::decode(const SPtr<DataStream>& stream, UINT32 dataLength,
		BinarySerializerFlags flags, SerializationContext* context, std::function<void(float)> progress)
	{
		mContext = context;
		mReportProgress = nullptr;
		mTotalBytesToRead = dataLength;
		mBuffer.seek(0);

		if (dataLength == 0)
		{
			if(mReportProgress)
				mReportProgress(1.0f);

			return nullptr;
		}

		const size_t start = stream->tell();
		const size_t end = start + dataLength;
		const size_t endBits = end * 8;
		mDecodeObjectMap.clear();

		bool hasMeta = !flags.isSet(BinarySerializerFlag::NoMeta);
		bool compress = flags.isSet(BinarySerializerFlag::Compress);

		BufferedBitstreamReader bufferedStream(&mBuffer, stream, PRELOAD_CHUNK_BYTES, FLUSH_AFTER_BYTES);

		// Note: Ideally we can avoid iterating twice over the stream data
		// Create empty instances of all ptr objects
		SPtr<IReflectable> rootObject = nullptr;
		do
		{
			UINT32 objectId = 0;
			UINT32 objectTypeId = 0;
			bool objectIsBaseClass = false;

			UINT32 bitsRead = readObjectMetaData(bufferedStream, flags, objectId, objectTypeId, objectIsBaseClass);
			bufferedStream.skip(-(int32_t)bitsRead);

			if (objectIsBaseClass)
			{
				BS_EXCEPT(InternalErrorException, "Encountered a base-class object while looking for a new object. " \
					"Base class objects are only supposed to be parts of a larger object.");
			}

			// TODO - Don't create object instance here as we might not know the type ID
			SPtr<IReflectable> object = IReflectable::createInstanceFromTypeId(objectTypeId);
			mDecodeObjectMap.insert(std::make_pair(objectId, ObjectToDecode(object, bufferedStream.tell())));

			if(rootObject == nullptr)
				rootObject = object;

		} while (decodeEntry(bufferedStream, endBits, flags, nullptr));

		assert(bufferedStream.tell() == endBits);

		// Don't set report callback until we actually do the reads
		mReportProgress = std::move(progress);
		bufferedStream.seek((uint32_t)start * 8);

		// Now go through all of the objects and actually decode them
		for(auto iter = mDecodeObjectMap.begin(); iter != mDecodeObjectMap.end(); ++iter)
		{
			ObjectToDecode& objToDecode = iter->second;

			if(objToDecode.isDecoded)
				continue;

			bufferedStream.seek((uint32_t)objToDecode.offset);

			objToDecode.decodeInProgress = true;
			// TODO - Assert if no object instance here (or just skip in case field was not found?)
			decodeEntry(bufferedStream, endBits, flags, objToDecode.object);
			objToDecode.decodeInProgress = false;
			objToDecode.isDecoded = true;
		}

		mDecodeObjectMap.clear();
		bufferedStream.seek(endBits);
		stream->seek(end);

		assert(bufferedStream.tell() == endBits);

		if(mReportProgress)
			mReportProgress(1.0f);

		return rootObject;
	}

	bool BinarySerializer::encodeEntry(IReflectable* object, UINT32 objectId, BufferedBitstreamWriter& stream, BinarySerializerFlags flags)
	{
		const bool writeMeta = !flags.isSet(BinarySerializerFlag::NoMeta);
		const bool compress = flags.isSet(BinarySerializerFlag::Compress);

		RTTITypeBase* rtti = object->getRTTI();
		bool isBaseClass = false;

		FrameStack<RTTITypeBase*> rttiInstances;

		const auto cleanup = [&]()
		{
			while (!rttiInstances.empty())
			{
				RTTITypeBase* rttiInstance = rttiInstances.top();
				rttiInstance->onSerializationEnded(object, mContext);
				mAlloc->destruct(rttiInstance);

				rttiInstances.pop();
			}
		};

		// If an object has base classes, we need to iterate through all of them
		do
		{
			RTTITypeBase* rttiInstance = rtti->_clone(*mAlloc);
			rttiInstances.push(rttiInstance);

			rttiInstance->onSerializationStarted(object, mContext);

			if (writeMeta)
			{
				// Encode object ID & type
				ObjectMetaData objectMetaData = encodeObjectMetaData(objectId, rtti->getRTTIId(), isBaseClass);
				stream.writeBytes(objectMetaData);
			}
			else
			{
				// Encode object ID
				UINT32 objectMetaData = encodeObjectMetaData(objectId, isBaseClass);

				if (compress)
					stream.writeVarInt(objectMetaData);
				else
					stream.writeBytes(objectMetaData);
			}

			const UINT32 numFields = rtti->getNumFields();
			for(UINT32 i = 0; i < numFields; i++)
			{
				RTTIField* curGenericField = rtti->getField(i);

				if (writeMeta)
				{
					// Copy field ID & other meta-data like field size and type
					int metaData = encodeFieldMetaData(curGenericField->schema, false);

					stream.writeBytes(metaData);
				}

				if(curGenericField->schema.isArray)
				{
					UINT32 arrayNumElems = curGenericField->getArraySize(rttiInstance, object);

					// Copy num vector elements
					if (compress)
						stream.writeVarInt(arrayNumElems);
					else
						stream.writeBytes(arrayNumElems);

					switch(curGenericField->schema.type)
					{
					case SerializableFT_ReflectablePtr:
						{
							auto* curField = static_cast<RTTIReflectablePtrFieldBase*>(curGenericField);

							for(UINT32 arrIdx = 0; arrIdx < arrayNumElems; arrIdx++)
							{
								SPtr<IReflectable> childObject;
								
								if (!flags.isSet(BinarySerializerFlag::Shallow))
									childObject = curField->getArrayValue(rttiInstance, object, arrIdx);

								UINT32 objId = registerObjectPtr(childObject);
								if (compress)
									stream.writeVarInt(objId);
								else
									stream.writeBytes(objId);
							}

							break;
						}
					case SerializableFT_Reflectable:
						{
							auto* curField = static_cast<RTTIReflectableFieldBase*>(curGenericField);

							for(UINT32 arrIdx = 0; arrIdx < arrayNumElems; arrIdx++)
							{
								IReflectable& childObject = curField->getArrayValue(rttiInstance, object, arrIdx);

								if(!complexTypeToStream(&childObject, stream, flags))
								{
									cleanup();
									return false;
								}
							}

							break;
						}
					case SerializableFT_Plain:
						{
							auto* curField = static_cast<RTTIPlainFieldBase*>(curGenericField);

							for(UINT32 arrIdx = 0; arrIdx < arrayNumElems; arrIdx++)
								curField->arrayElemToStream(rttiInstance, object, arrIdx, stream.getBitstream(), compress);

							break;
						}
					default:
						BS_LOG(Error, Serialization,
							"Error encoding data. Encountered a type I don't know how to encode. Type: {0}, Is array: {1}",
							curGenericField->schema.type, curGenericField->schema.isArray);
					}
				}
				else
				{
					switch(curGenericField->schema.type)
					{
					case SerializableFT_ReflectablePtr:
						{
							auto* curField = static_cast<RTTIReflectablePtrFieldBase*>(curGenericField);
							SPtr<IReflectable> childObject;
							
							if (!flags.isSet(BinarySerializerFlag::Shallow))
								childObject = curField->getValue(rttiInstance, object);

							UINT32 objId = registerObjectPtr(childObject);
							if (compress)
								stream.writeVarInt(objId);
							else
								stream.writeBytes(objId);

							break;
						}
					case SerializableFT_Reflectable:
						{
							auto* curField = static_cast<RTTIReflectableFieldBase*>(curGenericField);
							IReflectable& childObject = curField->getValue(rttiInstance, object);

							if(!complexTypeToStream(&childObject, stream, flags))
							{
								cleanup();
								return false;
							}

							break;
						}
					case SerializableFT_Plain:
						{
							auto* curField = static_cast<RTTIPlainFieldBase*>(curGenericField);
							curField->toStream(rttiInstance, object, stream.getBitstream(), compress);

							break;
						}
					case SerializableFT_DataBlock:
						{
							auto* curField = static_cast<RTTIManagedDataBlockFieldBase*>(curGenericField);

							UINT32 dataBlockSize = 0;
							SPtr<DataStream> blockStream = curField->getValue(rttiInstance, object, dataBlockSize);

							// Data block size
							if (compress)
								stream.writeVarInt(dataBlockSize);
							else
								stream.writeBytes(dataBlockSize);

							// Data block data
							auto dataToStore = (UINT8*)bs_stack_alloc(dataBlockSize);
							blockStream->read(dataToStore, dataBlockSize);

							stream.align();
							stream.writeBytes(dataToStore, dataBlockSize);
							bs_stack_free(dataToStore);

							break;
						}
					default:
						BS_LOG(Error, Serialization,
							"Error encoding data. Encountered a type I don't know how to encode. Type: {0}, Is array: {1}",
							curGenericField->schema.type, curGenericField->schema.isArray);
					}
				}

				stream.flush(false);
			}

			rtti = rtti->getBaseClass();
			isBaseClass = true;

		} while(rtti != nullptr); // Repeat until we reach the top of the inheritance hierarchy

		cleanup();

		return true;
	}

	bool BinarySerializer::decodeEntry(BufferedBitstreamReader& stream, size_t dataEnd, BinarySerializerFlags flags, const SPtr<IReflectable>& output)
	{
		const bool hasMeta = !flags.isSet(BinarySerializerFlag::NoMeta);
		const bool compressed = flags.isSet(BinarySerializerFlag::Compress);

		UINT32 objectId = 0;
		UINT32 objectTypeId = 0;
		bool objectIsBaseClass = false;

		readObjectMetaData(stream, flags, objectId, objectTypeId, objectIsBaseClass);

		if (objectIsBaseClass)
		{
			BS_EXCEPT(InternalErrorException, "Encountered a base-class object while looking for a new object. " \
				"Base class objects are only supposed to be parts of a larger object.");
		}

		RTTITypeBase* rtti = nullptr;
		if(output)
			rtti = output->getRTTI();

		FrameVector<RTTITypeBase*> rttiInstances;

		auto finalizeObject = [&rttiInstances, this](IReflectable* object)
		{
			// Note: It would make sense to finish deserializing derived classes before base classes, but some code
			// depends on the old functionality, so we'll keep it this way
			for (auto iter = rttiInstances.rbegin(); iter != rttiInstances.rend(); ++iter)
			{
				RTTITypeBase* curRTTI = *iter;

				curRTTI->onDeserializationEnded(object, mContext);
				mAlloc->destruct(curRTTI);
			}

			rttiInstances.clear();
		};

		RTTITypeBase* curRTTI = rtti;
		while (curRTTI)
		{
			RTTITypeBase* rttiInstance = curRTTI->_clone(*mAlloc);
			rttiInstances.push_back(rttiInstance);

			curRTTI = curRTTI->getBaseClass();
		}

		// Iterate in reverse to notify base classes before derived classes
		for(auto iter = rttiInstances.rbegin(); iter != rttiInstances.rend(); ++iter)
			(*iter)->onDeserializationStarted(output.get(), mContext);

		RTTITypeBase* rttiInstance = nullptr;
		UINT32 rttiInstanceIdx = 0;
		if(!rttiInstances.empty())
			rttiInstance = rttiInstances[0];

		while (stream.tell() < dataEnd)
		{
			bool isArray;
			SerializableFieldType fieldType;
			UINT16 fieldId;
			UINT8 fieldSize;
			bool hasDynamicSize;
			bool terminator;

			if(hasMeta)
			{
				UINT8 metaDataHeader = 0;
				stream.readBytes(metaDataHeader);
				stream.skipBytes(-(int32_t)sizeof(metaDataHeader));

				if (isObjectMetaData(metaDataHeader)) // We've reached a new object or a base class of the current one
				{
					UINT32 objId = 0;
					UINT32 objTypeId = 0;
					bool objIsBaseClass = false;
					UINT32 readBits = readObjectMetaData(stream, flags, objId, objTypeId, objIsBaseClass);

					// If it's a base class, get base class RTTI and handle that
					if (objIsBaseClass)
					{
						if (rtti != nullptr)
							rtti = rtti->getBaseClass();

						// Saved and current base classes don't match, so just skip over all that data
						if (rtti == nullptr || rtti->getRTTIId() != objTypeId)
							rtti = nullptr;

						rttiInstance = nullptr;

						if (rtti)
						{
							rttiInstance = rttiInstances[rttiInstanceIdx + 1];
							rttiInstanceIdx++;
						}

						continue;
					}
					else
					{
						// Found new object, we're done
						stream.skip(-(int32_t)readBits);

						finalizeObject(output.get());
						return true;
					}
				}

				if (compressed)
					terminator = isFieldTerminator(metaDataHeader);
				else
					terminator = false;

				if (!terminator)
					decodeFieldMetaData(metaDataHeader, fieldId, fieldSize, isArray, fieldType, hasDynamicSize, terminator);
			}
			else
			{
				// TODO - Need to read all the information from the schema or the current RTTI type:
				// - Check if we're reached a new base type and switch rtti
				// - If reached a new field read field information
				// - If reached terminator or end of object, finalize
				TODO;
			}

			if (terminator)
			{
				// We've processed the last field in this object, so return. Although we return false we don't actually know
				// if there is an object following this one. However it doesn't matter since terminator fields are only used
				// for embedded objects that are all processed within this method so we can compensate.
				finalizeObject(output.get());
				return false;
			}

			RTTIField* curGenericField = nullptr;

			if (rtti != nullptr)
				curGenericField = rtti->findField(fieldId);

			if (curGenericField != nullptr)
			{
				if (!hasDynamicSize && curGenericField->schema.size != fieldSize)
				{
					BS_EXCEPT(InternalErrorException,
						"Data type mismatch. Type size stored in file and actual type size don't match. ("
						+ toString(curGenericField->schema.size) + " vs. " + toString(fieldSize) + ")");
				}

				if (curGenericField->schema.isArray != isArray)
				{
					BS_EXCEPT(InternalErrorException,
						"Data type mismatch. One is array, other is a single type.");
				}

				if (curGenericField->schema.type != fieldType)
				{
					BS_EXCEPT(InternalErrorException,
						"Data type mismatch. Field types don't match. " + toString(UINT32(curGenericField->schema.type)) + " vs. " + toString(UINT32(fieldType)));
				}
			}

			UINT32 arrayNumElems = 1;
			if (isArray)
			{
				if (compressed)
					stream.readVarInt(arrayNumElems);
				else
					stream.readBytes(arrayNumElems);

				if(curGenericField != nullptr)
					curGenericField->setArraySize(rttiInstance, output.get(), arrayNumElems);

				switch (fieldType)
				{
				case SerializableFT_ReflectablePtr:
				{
					RTTIReflectablePtrFieldBase* curField = static_cast<RTTIReflectablePtrFieldBase*>(curGenericField);

					for (UINT32 i = 0; i < arrayNumElems; i++)
					{
						UINT32 childObjectId = 0;
						if (compressed)
							stream.readVarInt(childObjectId);
						else
							stream.readBytes(childObjectId);

						if (curField != nullptr)
						{
							auto findObj = mDecodeObjectMap.find(childObjectId);

							if(findObj == mDecodeObjectMap.end())
							{
								if(childObjectId != 0)
								{
									BS_LOG(Warning, Generic, "When deserializing, object ID: {0} was found but no such "
										"object was contained in the file.", childObjectId);
								}

								curField->setArrayValue(rttiInstance, output.get(), i, nullptr);
							}
							else
							{
								ObjectToDecode& objToDecode = findObj->second;

								const bool needsDecoding = !curField->schema.info.flags.isSet(RTTIFieldFlag::WeakRef) && !objToDecode.isDecoded;
								// TODO - Create objToDecode.object instance here, if not already created
								if (needsDecoding)
								{
									if (objToDecode.decodeInProgress)
									{
										BS_LOG(Warning, Generic, "Detected a circular reference when decoding. Referenced "
											"object's fields will be resolved in an undefined order (i.e. one of the "
											"objects will not be fully deserialized when assigned to its field). "
											"Use RTTI_Flag_WeakRef to get rid of this warning and tell the system which of"
											"the objects is allowed to be deserialized after it is assigned to its field.");
									}
									else
									{
										objToDecode.decodeInProgress = true;

										const uint32_t curOffset = stream.tell();
										stream.seek((uint32_t)objToDecode.offset);
										decodeEntry(stream, dataEnd, flags, objToDecode.object);
										stream.seek(curOffset);

										objToDecode.decodeInProgress = false;
										objToDecode.isDecoded = true;
									}
								}

								curField->setArrayValue(rttiInstance, output.get(), i, objToDecode.object);
							}
						}
					}

					break;
				}
				case SerializableFT_Reflectable:
				{
					RTTIReflectableFieldBase* curField = static_cast<RTTIReflectableFieldBase*>(curGenericField);

					for (UINT32 i = 0; i < arrayNumElems; i++)
					{
						SPtr<IReflectable> childObj;
						if(curField)
							childObj = curField->newObject();

						decodeEntry(stream, dataEnd, flags, childObj);

						if (curField != nullptr)
						{
							// Note: Would be nice to avoid this copy by value and decode directly into the field
							curField->setArrayValue(rttiInstance, output.get(), i, *childObj);
						}
					}
					break;
				}
				case SerializableFT_Plain:
				{
					RTTIPlainFieldBase* curField = static_cast<RTTIPlainFieldBase*>(curGenericField);

					for (UINT32 i = 0; i < arrayNumElems; i++)
					{
						UINT32 typeSize = fieldSize;
						if (hasDynamicSize)
						{
							if(compressed)
							{
								// TODO - Need to read size as BitLength encoded as var-int
								TODO;
							}
							else
							{
								stream.readBytes(typeSize);
								stream.skipBytes(-(int32_t)sizeof(uint32_t));
							}
						}

						if (curField != nullptr)
						{
							stream.preload(typeSize);
							curField->arrayElemFromBuffer(rttiInstance, output.get(), i, stream.getBitstream(), compressed);

							stream.skipBytes(typeSize); // TODO - Need to skip actual number of bits read in case of compressed structures (e.g. bool, var-int)
						}
						else
							stream.skipBytes(typeSize); // TODO - Need to skip actual number of bits encoded in case of compressed structured (e.g. bool, var-int)
					}
					break;
				}
				default:
					BS_EXCEPT(InternalErrorException,
						"Error decoding data. Encountered a type I don't know how to decode. Type: " + toString(UINT32(fieldType)) +
						", Is array: " + toString(isArray));
				}
			}
			else
			{
				switch (fieldType)
				{
				case SerializableFT_ReflectablePtr:
				{
					RTTIReflectablePtrFieldBase* curField = static_cast<RTTIReflectablePtrFieldBase*>(curGenericField);

					UINT32 childObjectId = 0;
					if (compressed)
						stream.readVarInt(childObjectId);
					else
						stream.readBytes(childObjectId);

					if (curField != nullptr)
					{
						auto findObj = mDecodeObjectMap.find(childObjectId);
						if(findObj == mDecodeObjectMap.end())
						{
							if(childObjectId != 0)
							{
								BS_LOG(Warning, Generic, "When deserializing, object ID: {0} was found but no such object "
									"was contained in the file.", childObjectId);
							}

							curField->setValue(rttiInstance, output.get(), nullptr);
						}
						else
						{
							ObjectToDecode& objToDecode = findObj->second;

							const bool needsDecoding = !curField->schema.info.flags.isSet(RTTIFieldFlag::WeakRef) && !objToDecode.isDecoded;
							// TODO - Create objToDecode.object instance here, if not already created
							if (needsDecoding)
							{
								if (objToDecode.decodeInProgress)
								{
									BS_LOG(Warning, Generic, "Detected a circular reference when decoding. Referenced "
										"object's fields will be resolved in an undefined order (i.e. one of the objects "
										"will not be fully deserialized when assigned to its field). Use "
										"RTTI_Flag_WeakRef to get rid of this warning and tell the system which of the "
										"objects is allowed to be deserialized after it is assigned to its field.");
								}
								else
								{
									objToDecode.decodeInProgress = true;

									const size_t curOffset = stream.tell();
									stream.seek((uint32_t)objToDecode.offset);
									decodeEntry(stream, dataEnd, flags, objToDecode.object);
									stream.seek((uint32_t)curOffset);

									objToDecode.decodeInProgress = false;
									objToDecode.isDecoded = true;
								}
							}

							curField->setValue(rttiInstance, output.get(), objToDecode.object);
						}
					}

					break;
				}
				case SerializableFT_Reflectable:
				{
					RTTIReflectableFieldBase* curField = static_cast<RTTIReflectableFieldBase*>(curGenericField);

					// Note: Ideally we can skip decoding the entry if the field no longer exists
					SPtr<IReflectable> childObj;
					if (curField)
						childObj = curField->newObject();

					decodeEntry(stream, dataEnd, flags, childObj);

					if (curField != nullptr)
					{
						// Note: Would be nice to avoid this copy by value and decode directly into the field
						curField->setValue(rttiInstance, output.get(), *childObj);
					}

					break;
				}
				case SerializableFT_Plain:
				{
					RTTIPlainFieldBase* curField = static_cast<RTTIPlainFieldBase*>(curGenericField);

					UINT32 typeSize = fieldSize;
					if (hasDynamicSize)
					{
						if(compressed)
						{
							// TODO - Need to read size as BitLength encoded as var-int
							TODO;
						}
						else
						{
							stream.readBytes(typeSize);
							stream.skipBytes(-(int32_t)sizeof(UINT32));
						}
					}

					if (curField != nullptr)
					{
						stream.preload(typeSize);
						curField->fromBuffer(rttiInstance, output.get(), stream.getBitstream());
						stream.skipBytes(typeSize);// TODO - Need to skip actual number of bits read in case of compressed structures (e.g. bool, var-int)
					}
					else
						stream.skipBytes(typeSize);// TODO - Need to skip actual number of bits read in case of compressed structures (e.g. bool, var-int)

					break;
				}
				case SerializableFT_DataBlock:
				{
					RTTIManagedDataBlockFieldBase* curField = static_cast<RTTIManagedDataBlockFieldBase*>(curGenericField);

					// Data block size
					UINT32 dataBlockSize = 0;
					if (compressed)
						stream.readVarInt(dataBlockSize);
					else
						stream.readBytes(dataBlockSize);

					stream.align();

					// Data block data
					if (curField != nullptr)
					{
						const SPtr<DataStream>& dataStream = stream.getDataStream();
						if (dataStream->isFile()) // Allow streaming
						{
							size_t curOffset = stream.tell();

							// Data blocks don't support data that isn't byte aligned, but encoder should take care of that
							assert((curOffset % 8) == 0);
							curOffset /= 8;
							
							dataStream->seek(curOffset);
							curField->setValue(rttiInstance, output.get(), dataStream, dataBlockSize);

							stream.skipBytes(dataBlockSize);
						}
						else
						{
							SPtr<MemoryDataStream> dataBlockStream = bs_shared_ptr_new<MemoryDataStream>(dataBlockSize);
							stream.readBytes(dataBlockStream->data(), dataBlockSize);

							curField->setValue(rttiInstance, output.get(), dataBlockStream, dataBlockSize);
						}
					}
					else
						stream.skipBytes(dataBlockSize);

					break;
				}
				default:
					BS_EXCEPT(InternalErrorException,
						"Error decoding data. Encountered a type I don't know how to decode. Type: " + toString(UINT32(fieldType)) +
						", Is array: " + toString(isArray));
				}

				stream.clearBuffered(false);
			}

			uint32_t bytesRead = Math::divideAndRoundUp(stream.tell(), 8U);
			if (mReportProgress && (bytesRead >= mNextProgressReport))
			{
				UINT32 lastReport = (bytesRead / REPORT_AFTER_BYTES) * REPORT_AFTER_BYTES;
				mNextProgressReport = lastReport + REPORT_AFTER_BYTES;

				mReportProgress(bytesRead / (float)mTotalBytesToRead);
			}
		}

		finalizeObject(output.get());
		return false;
	}

	bool BinarySerializer::complexTypeToStream(IReflectable* object, BufferedBitstreamWriter& stream, BinarySerializerFlags flags)
	{
		if (object != nullptr)
		{
			if (!encodeEntry(object, 0, stream, flags))
				return false;

			if (!flags.isSet(BinarySerializerFlag::NoMeta))
			{
				// Encode terminator field
				// Complex types require terminator fields because they can be embedded within other complex types and we need
				// to know when their fields end and parent's resume
				if(flags.isSet(BinarySerializerFlag::Compress))
				{
					UINT8 metaData = encodeFieldTerminator();
					stream.writeBytes(metaData);
				}
				else
				{
					int metaData = encodeFieldMetaData(RTTIFieldSchema(), true);
					stream.writeBytes(metaData);
				}
			}
		}

		return true;
	}

	UINT32 BinarySerializer::encodeFieldMetaData(const RTTIFieldSchema& schema, bool terminator)
	{
		// If O == 0 - Meta contains field information (Encoded using this method)
		//// Encoding: IIII IIII IIII IIII SSSS SSSS xTYP DCAO
		//// I - Id
		//// S - Size
		//// C - Complex
		//// A - Array
		//// D - Data block
		//// P - Complex ptr
		//// O - Object descriptor
		//// Y - Plain field has dynamic size
		//// T - Terminator (last field in an object)

		return (schema.id << 16 | schema.size << 8 |
			(schema.isArray ? 0x02 : 0) |
			((schema.type == SerializableFT_DataBlock) ? 0x04 : 0) |
			((schema.type == SerializableFT_Reflectable) ? 0x08 : 0) |
			((schema.type == SerializableFT_ReflectablePtr) ? 0x10 : 0) |
			(schema.hasDynamicSize ? 0x20 : 0) |
			(terminator ? 0x40 : 0)); // TODO - Low priority. Technically I could encode this much more tightly, and use var-ints for ID
	}

	void BinarySerializer::decodeFieldMetaData(UINT32 encodedData, UINT16& id, UINT8& size,
		bool& array, SerializableFieldType& type, bool& hasDynamicSize, bool& terminator)
	{
		if(isObjectMetaData(encodedData))
		{
			BS_EXCEPT(InternalErrorException,
				"Meta data represents an object description but is trying to be decoded as a field descriptor.");
		}

		terminator = (encodedData & 0x40) != 0;
		hasDynamicSize = (encodedData & 0x20) != 0;

		if((encodedData & 0x10) != 0)
			type = SerializableFT_ReflectablePtr;
		else if((encodedData & 0x08) != 0)
			type = SerializableFT_Reflectable;
		else if((encodedData & 0x04) != 0)
			type = SerializableFT_DataBlock;
		else
			type = SerializableFT_Plain;

		array = (encodedData & 0x02) != 0;
		size = (UINT8)((encodedData >> 8) & 0xFF);
		id = (UINT16)((encodedData >> 16) & 0xFFFF);
	}

	UINT8 BinarySerializer::encodeFieldTerminator()
	{
		// See the documentation for encodeFieldMetaData() on why we're using this format
		return 0x40;
	}

	bool BinarySerializer::isFieldTerminator(UINT8 data)
	{
		if(isObjectMetaData(data))
		{
			BS_EXCEPT(InternalErrorException,
				"Meta data represents an object description but is trying to be decoded as a field descriptor.");
		}

		return (data & 0x40) != 0;
	}

	BinarySerializer::ObjectMetaData BinarySerializer::encodeObjectMetaData(UINT32 objId, UINT32 objTypeId, bool isBaseClass)
	{
		// If O == 1 - Meta contains object instance information (Encoded using encodeObjectMetaData)
		//// Encoding: SSSS SSSS SSSS SSSS xxxx xxxx xxxx xxBO
		//// S - Size of the object identifier
		//// O - Object descriptor
		//// B - Base class indicator

		if(objId > 1073741823)
		{
			BS_EXCEPT(InvalidParametersException, "Object ID is larger than we can store (max 30 bits): " + toString(objId));
		}

		ObjectMetaData metaData;
		metaData.objectMeta = encodeObjectMetaData(objId, isBaseClass);
		metaData.typeId = objTypeId;
		return metaData;
	}

	void BinarySerializer::decodeObjectMetaData(ObjectMetaData encodedData, UINT32& objId, UINT32& objTypeId, bool& isBaseClass)
	{
		if(!isObjectMetaData(encodedData.objectMeta))
		{
			BS_EXCEPT(InternalErrorException,
				"Meta data represents a field description but is trying to be decoded as an object descriptor.");
		}

		decodeObjectMetaData(encodedData.objectMeta, objId, isBaseClass);
		objTypeId = encodedData.typeId;
	}

	UINT32 BinarySerializer::encodeObjectMetaData(UINT32 objId, bool isBaseClass)
	{
		// If O == 1 - Meta contains object instance information (Encoded using encodeObjectMetaData)
		//// Encoding: SSSS SSSS SSSS SSSS xxxx xxxx xxxx xxBO
		//// S - Size of the object identifier
		//// O - Object descriptor
		//// B - Base class indicator

		if(objId > 1073741823)
		{
			BS_EXCEPT(InvalidParametersException, "Object ID is larger than we can store (max 30 bits): " + toString(objId));
		}

		return (objId << 2) | (isBaseClass ? 0x02 : 0) | 0x01;
	}

	void BinarySerializer::decodeObjectMetaData(UINT32 encodedData, UINT32& objId, bool& isBaseClass)
	{
		if(!isObjectMetaData(encodedData))
		{
			BS_EXCEPT(InternalErrorException,
				"Meta data represents a field description but is trying to be decoded as an object descriptor.");
		}

		objId = (encodedData >> 2) & 0x3FFFFFFF;
		isBaseClass = (encodedData & 0x02) != 0;
	}

	bool BinarySerializer::isObjectMetaData(UINT32 encodedData)
	{
		return ((encodedData & 0x01) != 0);
	}

	UINT32 BinarySerializer::readObjectMetaData(BufferedBitstreamReader& stream, BinarySerializerFlags flags, uint32_t& objId, uint32_t& objTypeId, bool& isBaseType)
	{
		if (!flags.isSet(BinarySerializerFlag::NoMeta))
		{
			ObjectMetaData objectMetaData;
			objectMetaData.objectMeta = 0;
			objectMetaData.typeId = 0;

			if (stream.readBytes(objectMetaData) != sizeof(ObjectMetaData))
				BS_EXCEPT(InternalErrorException, "Error decoding data.");

			decodeObjectMetaData(objectMetaData, objId, objTypeId, isBaseType);
			return sizeof(ObjectMetaData) * 8;
		}
		else
		{
			UINT32 objectMetaData = 0;

			UINT32 bitsRead = 0;
			if (flags.isSet(BinarySerializerFlag::Compress))
				bitsRead = stream.readVarInt(objectMetaData);
			else
			{
				if (stream.readBytes(objectMetaData) != sizeof(objectMetaData))
					BS_EXCEPT(InternalErrorException, "Error decoding data.");

				bitsRead = sizeof(objectMetaData) * 8;
			}

			decodeObjectMetaData(objectMetaData, objId, isBaseType);
			// TODO - objTypeId needs to be retrieved here (either provided by user, from schema or from current RTTIType)
			objTypeId = TODO;

			return bitsRead;
		}
	}

	UINT32 BinarySerializer::findOrCreatePersistentId(IReflectable* object)
	{
		void* ptrAddress = (void*)object;

		auto findIter = mObjectAddrToId.find(ptrAddress);
		if(findIter != mObjectAddrToId.end())
			return findIter->second;

		UINT32 objId = mLastUsedObjectId++;
		mObjectAddrToId.insert(std::make_pair(ptrAddress, objId));

		return objId;
	}

	UINT32 BinarySerializer::registerObjectPtr(SPtr<IReflectable> object)
	{
		if(object == nullptr)
			return 0;

		void* ptrAddress = (void*)object.get();

		auto iterFind = mObjectAddrToId.find(ptrAddress);
		if(iterFind == mObjectAddrToId.end())
		{
			UINT32 objId = findOrCreatePersistentId(object.get());

			mObjectsToEncode.push_back(ObjectToEncode(objId, object));
			mObjectAddrToId.insert(std::make_pair(ptrAddress, objId));

			return objId;
		}

		return iterFind->second;
	}
}

#undef COPY_TO_BUFFER
