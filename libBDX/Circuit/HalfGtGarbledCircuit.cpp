#include "HalfGtGarbledCircuit.h"
#include "cryptoTools/Network/Channel.h"
#include "cryptoTools/Common/ByteStream.h"
#include "cryptoTools/Common/Log.h"
#include <cassert>

namespace osuCrypto 
{


	HalfGtGarbledCircuit::HalfGtGarbledCircuit(HalfGtGarbledCircuit&& src)
		:
		mGlobalOffset(src.mGlobalOffset),
		mInputWires(std::move(src.mInputWires)),
		mOutputWires(std::move(src.mOutputWires)),
		mGates(std::move(src.mGates)),
		mTranslationTable(std::move(src.mTranslationTable))
	{

	}


	HalfGtGarbledCircuit::~HalfGtGarbledCircuit()
	{
	}

	void HalfGtGarbledCircuit::GarbleSend(
		const Circuit & cd, 
		const block & seed, 
		Channel & chl,
		std::vector<block>& wires,
		const std::vector<block>& indexArray
#ifdef ADAPTIVE_SECURE 
		, std::vector<block> tableMasks
#endif
		)
	{
		//mGates.reserve(cd.NonXorGateCount());

		mInputWires.clear();
		mInputWires.resize(cd.InputWireCount());

		mOutputWires.clear();
		//mGates.clear();

		AES aesSeedKey(seed);

		//create the delta for the free Xor. Encrypt zero twice. We get a good enough random delta by encrypting twice
		aesSeedKey.ecbEncBlock(ZeroBlock, mGlobalOffset);
		aesSeedKey.ecbEncBlock(mGlobalOffset, mGlobalOffset);
		ByteArray(mGlobalOffset)[0] |= 1; // make sure the bottom bit is a 1 for point-n-permute

		// Compute the input labels as AES permutations on mIndexArray
		mInputWires.resize(cd.InputWireCount());
		aesSeedKey.ecbEncBlocks(indexArray.data(), mInputWires.size(), (block*)mInputWires.data());
		std::copy(mInputWires.begin(), mInputWires.end(), wires.begin());

		block temp[4], hash[4], tweaks[2]{ ZeroBlock, _mm_set_epi64x(0, cd.Gates().size()) };
		u8 aPermuteBit, bPermuteBit;

		auto buff = std::unique_ptr<ByteStream>(new ByteStream(sizeof(GarbledGate<2>) * cd.NonXorGateCount()));
		buff->setp(sizeof(GarbledGate<2>) * cd.NonXorGateCount());
		std::array<block, 2> zeroAndGlobalOffset{ { ZeroBlock, mGlobalOffset } };


		//std::cout << "cir size " << buff->size() << std::endl;
		auto gateIter = reinterpret_cast<GarbledGate<2>*>(buff->data());
#ifdef ADAPTIVE_SECURE 
		auto maskIter = tableMasks.begin();
#endif

		for (const auto& gate : cd.Gates())
		{
			//mInputWires.emplace_back();
			auto& c = wires[gate.mOutput];
			auto& a = wires[gate.mInput[0]];
			auto& b = wires[gate.mInput[1]];
			auto gt = gate.Type();

//#ifndef NDEBUG
//			if (gt == GateType::Zero || gt == GateType::One || gt == GateType::na || gt == GateType::nb || gt == GateType::a || gt == GateType::b)
//				throw std::runtime_error("Constant/unary gates not supported");
//#endif
			if (gt == GateType::Xor || gt == GateType::Nxor) {
				c = a ^ b ^ zeroAndGlobalOffset[(u8)gt & 1];
			}
			else {
				// generate the garbled table
				auto& garbledTable = gateIter++->mGarbledTable;


				// compute the gate modifier variables
				auto& aAlpha = gate.AAlpha();
				auto& bAlpha = gate.BAlpha();
				auto& cAlpha = gate.CAlpha();

				//signal bits of wire 0 of input0 and wire 0 of input1
				aPermuteBit = PermuteBit(a);
				bPermuteBit = PermuteBit(b);

				// compute the hashs of the wires as H(x) = AES_f( x * 2 ^ tweak) ^ (x * 2 ^ tweak)    
				hash[0] = _mm_slli_epi64(a, 1) ^ tweaks[0];
				hash[1] = _mm_slli_epi64((a ^ mGlobalOffset), 1) ^ tweaks[0];
				hash[2] = _mm_slli_epi64(b, 1) ^ tweaks[1];
				hash[3] = _mm_slli_epi64((b ^ mGlobalOffset), 1) ^ tweaks[1];
			    mAesFixedKey.ecbEncFourBlocks(hash, temp);
				hash[0] = hash[0] ^ temp[0]; // H( a0 )
				hash[1] = hash[1] ^ temp[1]; // H( a1 )
				hash[2] = hash[2] ^ temp[2]; // H( b0 )
				hash[3] = hash[3] ^ temp[3]; // H( b1 )

											 // increment the tweaks
				tweaks[0] = tweaks[0] + OneBlock;
				tweaks[1] = tweaks[1] + OneBlock;

				// compute the table entries
				garbledTable[0] = hash[0] ^ hash[1];
				garbledTable[1] = hash[2] ^ hash[3] ^ a;
				if (bAlpha ^ bPermuteBit) garbledTable[0] = garbledTable[0] ^ mGlobalOffset;
				if (aAlpha)               garbledTable[1] = garbledTable[1] ^ mGlobalOffset;


				// compute the table entries
				garbledTable[0] = hash[0] ^ hash[1] ^ zeroAndGlobalOffset[bAlpha ^ bPermuteBit];
				garbledTable[1] = hash[2] ^ hash[3] ^ a ^ zeroAndGlobalOffset[aAlpha];

				c = hash[aPermuteBit] ^
					hash[2 ^ bPermuteBit] ^
					zeroAndGlobalOffset[((aPermuteBit ^ aAlpha) && (bPermuteBit ^ bAlpha)) ^ cAlpha];


#ifdef ADAPTIVE_SECURE 
				garbledTable[0] = garbledTable[0] ^ *maskIter++;
				garbledTable[1] = garbledTable[1] ^ *maskIter++;
#endif
			}
		}

        if (buff->size())
        {
            chl.asyncSend(std::move(buff));
        }

		mOutputWires.resize(cd.Outputs().size());
		mTranslationTable.reset(cd.OutputCount());
		for (u64 i = 0; i < cd.Outputs().size(); ++i)
		{
			mOutputWires[i] = (wires[cd.Outputs()[i]]);
			mTranslationTable[i] = PermuteBit(wires[cd.Outputs()[i]]);
		}

        if (mTranslationTable.size())
        {
            chl.asyncSendCopy(mTranslationTable);
        }

#ifdef STRONGEVAL
		mInternalWires = wires;
#endif
	}



	void HalfGtGarbledCircuit::Garble(const Circuit& cd, const block& seed, const std::vector<block>& indexArray
#ifdef ADAPTIVE_SECURE 
		, std::vector<block> tableMasks
#endif
		)
	{

		assert(indexArray.size() >= mInputWires.size());
		assert(eq(indexArray[0], ZeroBlock));

		mGates.reserve(cd.NonXorGateCount());

		mInputWires.clear();
		mInputWires.reserve(cd.WireCount());

		mOutputWires.clear();
		mGates.clear();

		AES aesSeedKey(seed);

		//create the delta for the free Xor. Encrypt zero twice. We get a good enough random delta by encrypting twice
		aesSeedKey.ecbEncBlock(ZeroBlock, mGlobalOffset);
		aesSeedKey.ecbEncBlock(mGlobalOffset, mGlobalOffset);
		ByteArray(mGlobalOffset)[0] |= 1; // make sure the bottom bit is a 1 for point-n-permute

		// Compute the input labels as AES permutations on mIndexArray
		mInputWires.resize(cd.InputWireCount());
		aesSeedKey.ecbEncBlocks(indexArray.data(), mInputWires.size(), (block*)mInputWires.data());

		block temp[4], hash[4], tweaks[2]{ ZeroBlock, _mm_set_epi64x(0, cd.Gates().size()) };
		u8 aPermuteBit, bPermuteBit;

		std::array<block, 2> zeroAndGlobalOffset{ {ZeroBlock, mGlobalOffset} };

#ifdef ADAPTIVE_SECURE 
		auto maskIter = tableMasks.begin();
#endif

		for (const auto& gate : cd.Gates())
		{
			mInputWires.emplace_back();
			auto& c = mInputWires.back();
			auto& a = mInputWires[gate.mInput[0]];
			auto& b = mInputWires[gate.mInput[1]];
			auto gt = gate.Type();

#ifndef NDEBUG
			//if (gt == GateType::Zero || gt == GateType::One || gt == GateType::na || gt == GateType::nb || gt == GateType::a || gt == GateType::b )
			//	throw std::runtime_error("Constant/unary gates not supported");
#endif
			if (gt == GateType::Xor || gt == GateType::Nxor) {
				c = a ^ b ^ zeroAndGlobalOffset[ (u8)gt & 1];
			}
			else {
				// generate the garbled table
				mGates.emplace_back();
				auto& garbledTable = mGates.back().mGarbledTable;

				// compute the gate modifier variables
				auto& aAlpha = gate.AAlpha();
				auto& bAlpha = gate.BAlpha();
				auto& cAlpha = gate.CAlpha();

				//signal bits of wire 0 of input0 and wire 0 of input1
				aPermuteBit = PermuteBit(a);
				bPermuteBit = PermuteBit(b);

				// compute the hashs of the wires as H(x) = AES_f( x * 2 ^ tweak) ^ (x * 2 ^ tweak)    
				hash[0] = _mm_slli_epi64(a, 1) ^ tweaks[0];
				hash[1] = _mm_slli_epi64((a ^ mGlobalOffset), 1) ^ tweaks[0];
				hash[2] = _mm_slli_epi64(b, 1) ^ tweaks[1];
				hash[3] = _mm_slli_epi64((b ^ mGlobalOffset), 1) ^ tweaks[1];
                mAesFixedKey.ecbEncFourBlocks( hash, temp);
				hash[0] = hash[0] ^ temp[0]; // H( a0 )
				hash[1] = hash[1] ^ temp[1]; // H( a1 )
				hash[2] = hash[2] ^ temp[2]; // H( b0 )
				hash[3] = hash[3] ^ temp[3]; // H( b1 )

				// increment the tweaks
				tweaks[0] = tweaks[0] + OneBlock;
				tweaks[1] = tweaks[1] + OneBlock;

				// compute the table entries
				garbledTable[0] = hash[0] ^ hash[1] ^ zeroAndGlobalOffset[bAlpha ^ bPermuteBit];
				garbledTable[1] = hash[2] ^ hash[3] ^ a ^ zeroAndGlobalOffset[aAlpha];

				c = hash[aPermuteBit] ^
					hash[2 ^ bPermuteBit] ^
					zeroAndGlobalOffset[((aPermuteBit ^ aAlpha) && (bPermuteBit ^ bAlpha)) ^ cAlpha];
#ifdef ADAPTIVE_SECURE 
				garbledTable[0] = garbledTable[0] ^ *maskIter++;
				garbledTable[1] = garbledTable[1] ^ *maskIter++;
#endif
			}
		}

		mOutputWires.reserve(cd.Outputs().size());
		mTranslationTable.reset(cd.OutputCount());
		for (u64 i = 0; i < cd.Outputs().size(); ++i)
		{
			mOutputWires.push_back(mInputWires[cd.Outputs()[i]]);
			mTranslationTable[i] = PermuteBit(mInputWires[cd.Outputs()[i]]);
		}


#ifdef STRONGEVAL
		mInternalWires = mInputWires;
#endif
		mInputWires.resize(cd.InputWireCount());
		mInputWires.shrink_to_fit();
	}

	void HalfGtGarbledCircuit::evaluate(const Circuit& cd, std::vector<block>& labels
#ifdef ADAPTIVE_SECURE 
		, std::vector<block> tableMasks
#endif
		)
	{
#ifndef NDEBUG
		if (labels.size() != cd.WireCount())
			throw std::runtime_error("");
#endif

#ifdef STRONGEVAL
		BitVector values(cd.WireCount());
		for (u64 i = 0; i < labels.size(); ++i)
		{
			if (labels[i] == mInternalWires[i].mZeroLabel)
				values[i] = 0;
			else if (labels[i] == (mInternalWires[i].mZeroLabel ^ mGlobalOffset))
				values[i] = 1;
			else throw std::runtime_error("");
		}
#endif
		auto garbledGateIter = mGates.begin(); 
#ifdef ADAPTIVE_SECURE 
		auto maskIter = tableMasks.begin();
#endif
		block temp[2], hashs[2], tweaks[2]{ ZeroBlock, _mm_set_epi64x(0, cd.Gates().size()) };

		for (const auto& gate : cd.Gates()) {
			auto& a = labels[gate.mInput[0]];
			auto& b = labels[gate.mInput[1]];
			auto& c = labels[gate.mOutput];
			auto& gt = gate.Type();

			if (gt == GateType::Xor || gt == GateType::Nxor) {
				c = a ^ b;
			}
			else {
#ifdef ADAPTIVE_SECURE 
				auto& maskedGT = garbledGateIter++->mGarbledTable;
				block* garbledTable = &(*maskIter);
				maskIter += 2;

				garbledTable[0] = garbledTable[0] ^ maskedGT[0];
				garbledTable[1] = garbledTable[1] ^ maskedGT[1];
#else 
				auto& garbledTable = garbledGateIter++->mGarbledTable;
#endif

				// compute the hashs
				hashs[0] = _mm_slli_epi64(a, 1) ^ tweaks[0];
				hashs[1] = _mm_slli_epi64(b, 1) ^ tweaks[1];
                mAesFixedKey.ecbEncTwoBlocks(hashs, temp);
				hashs[0] = temp[0] ^ hashs[0]; // a
				hashs[1] = temp[1] ^ hashs[1]; // b

				// increment the tweaks
				tweaks[0] = tweaks[0] + OneBlock;
				tweaks[1] = tweaks[1] + OneBlock;

				// compute the output wire label
				c = hashs[0] ^ hashs[1];
				if (PermuteBit(a)) c = c ^ garbledTable[0];
				if (PermuteBit(b)) c = c ^ garbledTable[1] ^ a;
			}

#ifdef STRONGEVAL
			u8 aVal = values[gate.mInput[0]] ? 1 : 0;
			u8 bVal = values[gate.mInput[1]] ? 2 : 0;
			values[gate.mOutput] = gate.mLogicTable[aVal | bVal];

			if (values[gate.mOutput])
			{
				if (c != mInternalWires[gate.mOutput]^(mGlobalOffset))
					throw std::runtime_error("");
			}
			else if (c != mInternalWires[gate.mOutput].mZeroLabel)
				throw std::runtime_error("");
#endif
		}
	}

	bool HalfGtGarbledCircuit::Validate(const Circuit& cd, const block& seed, const std::vector<block>& indexArray
#ifdef ADAPTIVE_SECURE 
		, std::vector<block> tableMasks
#endif
		)
	{

		
		AES aesSeedKey(seed);

		mInputWires.clear();
		mInputWires.reserve(cd.WireCount());


		//create the delta for the free Xor. Encrypt zero twice. We get a good enough random delta by encrypting twice
        aesSeedKey.ecbEncBlock(ZeroBlock, mGlobalOffset);
        aesSeedKey.ecbEncBlock(mGlobalOffset, mGlobalOffset);
		ByteArray(mGlobalOffset)[0] |= 1; // make sure the bottom bit is a 1 for point-n-permute

		// Compute the input labels as AES permutations on mIndexArray
		mInputWires.resize(cd.InputWireCount());
        aesSeedKey.ecbEncBlocks(indexArray.data(), mInputWires.size(), (block*)mInputWires.data());

		block temp[4], hash[4], tweaks[2]{ ZeroBlock, _mm_set_epi64x(0, cd.Gates().size()) };
		u8 aPermuteBit, bPermuteBit;
		GarbledGate<2> garbledGate;

		// generate the garbled table
		auto& garbledTable = garbledGate.mGarbledTable;

		std::vector<GarbledGate<2>>::const_iterator gateIter = mGates.begin();
#ifdef ADAPTIVE_SECURE 
		auto maskIter = tableMasks.begin();
#endif
		std::array<block, 2> zeroAndGlobalOffset{ { ZeroBlock, mGlobalOffset } };


		for (const auto& gate : cd.Gates())
		{
			mInputWires.emplace_back();
			auto& c = mInputWires.back();
			auto& a = mInputWires[gate.mInput[0]];
			auto& b = mInputWires[gate.mInput[1]];
			auto gt = gate.Type();

#ifndef NDEBUG
			if (gt == GateType::Zero || gt == GateType::One || gt == GateType::na || gt == GateType::nb || gt == GateType::a || gt == GateType::b)
				throw std::runtime_error("Constant/unary gates not supported");
#endif
			if (gt == GateType::Xor || gt == GateType::Nxor) {
				c = a ^ b ^ zeroAndGlobalOffset[(u8)gt & 1];
			}
			else {

				// compute the gate modifier variables
				auto& aAlpha = gate.AAlpha();
				auto& bAlpha = gate.BAlpha();
				auto& cAlpha = gate.CAlpha();

				//signal bits of wire 0 of input0 and wire 0 of input1
				aPermuteBit = PermuteBit(a);
				bPermuteBit = PermuteBit(b);

				// compute the hashs of the wires ROUGHLY (<< op loses bit shift 64<-63)
				// as    H(x) = AES_f( x * 2 ^ tweak) ^ (x * 2 ^ tweak)    
				hash[0] = _mm_slli_epi64(a, 1) ^ tweaks[0];
				hash[1] = _mm_slli_epi64((a ^ mGlobalOffset), 1) ^ tweaks[0];
				hash[2] = _mm_slli_epi64(b, 1) ^ tweaks[1];
				hash[3] = _mm_slli_epi64((b ^ mGlobalOffset), 1) ^ tweaks[1];
                mAesFixedKey.ecbEncFourBlocks(hash, temp);
				hash[0] = hash[0] ^ temp[0]; // H( a0 )
				hash[1] = hash[1] ^ temp[1]; // H( a1 )
				hash[2] = hash[2] ^ temp[2]; // H( b0 )
				hash[3] = hash[3] ^ temp[3]; // H( b1 )

				// increment the tweaks
				tweaks[0] = tweaks[0] + OneBlock;
				tweaks[1] = tweaks[1] + OneBlock;

				// compute the table entries
				garbledTable[0] = hash[0] ^ hash[1] ^ zeroAndGlobalOffset[bAlpha ^ bPermuteBit];
				garbledTable[1] = hash[2] ^ hash[3] ^ a ^ zeroAndGlobalOffset[aAlpha];

				c = hash[aPermuteBit] ^
					hash[2 ^ bPermuteBit] ^
					zeroAndGlobalOffset[((aPermuteBit ^ aAlpha) && (bPermuteBit ^ bAlpha)) ^ cAlpha];


#ifdef ADAPTIVE_SECURE 
				garbledTable[0] = garbledTable[0] ^ *maskIter++;
				garbledTable[1] = garbledTable[1] ^ *maskIter++;
#endif

				if (neq(garbledTable[0], gateIter->mGarbledTable[0]))
					throw std::runtime_error("GC check failed");
				if (neq(garbledTable[1], gateIter->mGarbledTable[1]))
					throw std::runtime_error("GC check failed");

				++gateIter;
			}
		}

		mOutputWires.clear();
		mOutputWires.reserve(cd.Outputs().size());
		for (u64 i = 0; i < cd.Outputs().size(); ++i)
		{
			mOutputWires.push_back(mInputWires[cd.Outputs()[i]]);
			if (mTranslationTable[i] != PermuteBit(mInputWires[cd.Outputs()[i]]))
				throw std::runtime_error("GC check failed");
		}

		mInputWires.resize(cd.InputWireCount());
		mInputWires.shrink_to_fit();

		return true;
	}
	//#define STRONG_TRANSLATE
	void HalfGtGarbledCircuit::translate(const Circuit& cd, std::vector<block>&  labels, BitVector& output)
	{
		output.reset(mTranslationTable.size());

		for (u64 i = 0; i < cd.OutputCount(); ++i)
		{
#ifdef STRONG_TRANSLATE
			if (labels[wire] == mInputWires[wire].mZeroLabel)
			{
				output.push_back(false);
			}
			else  if (labels[wire] == mInputWires[wire]^(mGlobalOffset))
			{
				output.push_back(true);
			}
			else
			{
				throw std::runtime_error("Wire didnt match");
			}
#else
			output[i] = (mTranslationTable[i] ^ PermuteBit(labels[cd.Outputs()[i]]));
#endif

            if (cd.mOutputInverts[i])
                output[cd.Outputs()[i]] = !output[cd.Outputs()[i]];
		}


	}


	void HalfGtGarbledCircuit::SendToEvaluator(Channel & channel)
	{

		//for (u64 i = 0; i < mGates.size(); ++i)
		//{
		//	buff->append(ByteArray(mGates[i].mGarbledTable[0]), sizeof(block));
		//	buff->append(ByteArray(mGates[i].mGarbledTable[1]), sizeof(block));
		//}

        if (mGates.size())
        {
            auto buff = std::unique_ptr<ByteStream>(new ByteStream((u8*)mGates.data(), mGates.size() * sizeof(GarbledGate<2>)));
            channel.asyncSend(std::move(buff));
        }
		
		//mTranslationTable.pack(*buff);
        if (mTranslationTable.size())
        {
    		channel.send(mTranslationTable);
        }
	}

	void HalfGtGarbledCircuit::ReceiveFromGarbler(const Circuit& cd, Channel & channel)
	{
		mGates.resize(cd.NonXorGateCount());
		
        if (mGates.size())
        {
            channel.recv(mGates.data(), mGates.size() * sizeof(GarbledGate<2>));
        }

		mTranslationTable.reset(cd.OutputCount());

        if (cd.OutputCount())
        {
    		channel.recv(mTranslationTable.data(), mTranslationTable.sizeBytes());
        }
	}

}
