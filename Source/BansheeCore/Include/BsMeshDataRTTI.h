//********************************** Banshee Engine (www.banshee3d.com) **************************************************//
//**************** Copyright (c) 2016 Marko Pintera (marko.pintera@gmail.com). All rights reserved. **********************//
#pragma once

#include "BsCorePrerequisites.h"
#include "BsRTTIType.h"
#include "BsMeshData.h"
#include "BsVertexDeclaration.h"
#include "BsDataStream.h"

namespace BansheeEngine
{
	/** @cond RTTI */
	/** @addtogroup RTTI-Impl-Core
	 *  @{
	 */

	BS_ALLOW_MEMCPY_SERIALIZATION(IndexType);

	class BS_CORE_EXPORT MeshDataRTTI : public RTTIType<MeshData, GpuResourceData, MeshDataRTTI>
	{
	private:
		SPtr<VertexDataDesc> getVertexData(MeshData* obj) { return obj->mVertexData; }
		void setVertexData(MeshData* obj, SPtr<VertexDataDesc> value) { obj->mVertexData = value; }

		IndexType& getIndexType(MeshData* obj) { return obj->mIndexType; }
		void setIndexType(MeshData* obj, IndexType& value) { obj->mIndexType = value; }

		UINT32& getNumVertices(MeshData* obj) { return obj->mNumVertices; }
		void setNumVertices(MeshData* obj, UINT32& value) { obj->mNumVertices = value; }

		UINT32& getNumIndices(MeshData* obj) { return obj->mNumIndices; }
		void setNumIndices(MeshData* obj, UINT32& value) { obj->mNumIndices = value; }

		SPtr<DataStream> getData(MeshData* obj, UINT32& size)
		{
			size = obj->getInternalBufferSize();

			return bs_shared_ptr_new<MemoryDataStream>(obj->getData(), size, false);
		}

		void setData(MeshData* obj, const SPtr<DataStream>& value, UINT32 size)
		{
			obj->allocateInternalBuffer(size);
			value->read(obj->getData(), size);
		}

	public:
		MeshDataRTTI()
		{
			addReflectablePtrField("mVertexData", 0, &MeshDataRTTI::getVertexData, &MeshDataRTTI::setVertexData);

			addPlainField("mIndexType", 1, &MeshDataRTTI::getIndexType, &MeshDataRTTI::setIndexType);
			addPlainField("mNumVertices", 2, &MeshDataRTTI::getNumVertices, &MeshDataRTTI::setNumVertices);
			addPlainField("mNumIndices", 3, &MeshDataRTTI::getNumIndices, &MeshDataRTTI::setNumIndices);

			addDataBlockField("data", 4, &MeshDataRTTI::getData, &MeshDataRTTI::setData, 0);
		}

		SPtr<IReflectable> newRTTIObject() override
		{
			return bs_shared_ptr<MeshData>(new (bs_alloc<MeshData>()) MeshData());
		}

		const String& getRTTIName() override
		{
			static String name = "MeshData";
			return name;
		}

		UINT32 getRTTIId() override
		{
			return TID_MeshData;
		}
	};

	/** @} */
	/** @endcond */
}