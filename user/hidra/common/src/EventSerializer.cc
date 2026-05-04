#include "EventSerializer.hh"
#include "HidraUtils.hh"

#include <fstream>
#include <stdexcept>


namespace hidra {

  namespace {

    // LITTLE ENDIAN
    template <typename T>
    void appendLE(std::vector<std::uint8_t> &buffer, T value)
    {
      static_assert(std::is_integral<T>::value, "appendIntegerLE requires an integer type");
     
      using UnsignedT = typename std::make_unsigned<T>::type;
      const UnsignedT v = static_cast<UnsignedT>(value);
      
      for(std::size_t i = 0; i < sizeof(T); ++i) {
	buffer.push_back( static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF) );
      }
    }
    template <typename T>
    void writeLE(std::vector<std::uint8_t> &buffer, std::size_t offset, T value)
    {
      using UnsignedT = typename std::make_unsigned<T>::type;
      const UnsignedT v = static_cast<UnsignedT>(value);
      
      for(std::size_t i = 0; i < sizeof(T); ++i) {
	buffer[offset + i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
      }
    }


    // BIG ENDIAN
    template <typename T>
    void appendBE(std::vector<std::uint8_t> &buffer, T value)
    {
      static_assert(std::is_integral<T>::value, "appendIntegerBE requires an integer type");

      using UnsignedT = typename std::make_unsigned<T>::type;
      const UnsignedT v = static_cast<UnsignedT>(value);
      
      for(std::size_t i = 0; i < sizeof(T); ++i) {
	const std::size_t shift = 8 * (sizeof(T) - 1 - i);
	buffer.push_back( static_cast<std::uint8_t>((v >> shift) & 0xFF));
      }
    }
 

  } // end of anonymous namespace

  std::vector<std::uint8_t> EventSerializer::Serialize(const eudaq::Event &event){

	std::vector<std::uint8_t> buffer;

	// EVENT HEADER:
	// makrer (16 bit) [0, 1]
	// header size (32 bit) [2,3,4,5]
	// trailer size (32 bit) [6,7,8,9] 
	// event size (including header and trailer) (32 bit) [10,11,12,13]
	// event number (32 bit) [14,15,16,17]
	// spill number (32 bit) [18,19,20,21]
	// eventTime (64 bit) [22,23,24,25,26,27,28,29]
	// triggerMask (8 bit) [30]
	// reserved (64 bit) [31,32,..38]
	// reserved (32 bit) [39,40,41,42]
	// DetectorMask (16 bit) [43,44]
	// ------------> dynamic
	// sizeXDC (32 bit) [45..48]
	// sizeFERS (32 bit) [49..52]
	// sizeCrystal (32 bit) [53..56]
	// sizeTracker (32 bit) [57..60]
	// <------------
	// end marker (16 bit) [61,62]

	// FOR EACH SUBDETECTOR (IF PRESENT):
	// detEvent marker (16 bit) [0,1]
	// DetID (8 bit) [2]
	// event number (32 bit) [3,4,5,6]
	// spill number (0x0 if not applicable) (32 bit) [7,8,9,10]
	// eventTime1 (64 bit) [11..18]
	// eventTime2 (64 bit) [19..16]
	// reserved (32 bit) [17..20]
	// Blocks (payload)
	// detEventEndMarker (16 bit)

	// EVENT TRAILER
	// marker (16 bit)
	// sanity (8 bit) 
	
	

	const std::uint16_t EVENT_MARKER = 0xB0B0;
	const std::uint16_t EVENT_HEADER_ENDMARKER = 0xBBBB;
	const std::uint16_t EVENT_TRAILER = 0xD04E;
	const std::uint16_t DETECTOR_EVENT_MARKER = 0xDEDE;
	const std::uint16_t DETECTOR_EVENT_ENDMARKER = 0xDDDD;
	uint8_t placeholder8 = 0xFF;
	uint16_t placeholder16 = 0xFFFF;
	uint32_t placeholder32 = 0xFFFFFFFF;
	uint64_t placeholder64 = (placeholder32 << 31) | placeholder32;
	const uint32_t TrailerSize = 2;


	// TO BE IMPLEMENTED IN HIDRA EUDAQ EVENTS
	uint32_t SpillNumber = 0;
	
	appendLE(buffer, EVENT_MARKER);
	appendLE(buffer, placeholder32); // header size
	appendLE(buffer, TrailerSize);
	appendLE(buffer, placeholder32); // event size
	appendLE(buffer, static_cast<std::uint32_t>(event.GetTriggerN())); 
	appendLE(buffer, SpillNumber);
	appendLE(buffer, static_cast<std::uint64_t>(event.GetTimestampBegin()));
	appendLE(buffer, static_cast<std::uint8_t>(std::stoul(event.GetTag("triggerMask"))));
	appendLE(buffer, placeholder64); // reserved
	appendLE(buffer, placeholder32); // reserved
	appendLE(buffer, placeholder16); // detector mask

	int NSources = std::stoi(event.GetTag("N_SOURCES"));

	for (int is = 0; is < NSources; is++){
	  appendLE(buffer, placeholder32); // data size for the subdetector
	}

	appendLE(buffer, EVENT_HEADER_ENDMARKER);

	uint32_t HeaderSize = buffer.size();
	writeLE(buffer, 2, HeaderSize);

	uint16_t detMask = 0x0000;

	std::vector<std::string> seen_producers{};

	// SERIALIZING SUB-EVENTS
	// first loop to order them and make the mask
	for (int is=0; is < NSources; is++){
	  std::string producerName = event.GetSubEvent(is)->GetTag("Producer");
	  seen_producers.push_back(producerName);
	  int detID = 0;
	  auto itProd = hidra::utils::Producers.find(producerName);
	  if (itProd != hidra::utils::Producers.end()){
	    detID = itProd->second;
	  }
	  else {
	    HIDRA_ERROR("Detector {} is not in the predefined list. This will not be tolerated in production", producerName);
	  }
	  
	  detMask |= (1 << detID);
	}

	writeLE(buffer, 43, detMask);

	// reordering sub-events according to hidra::utils::Producers map
	std::vector<int> idx(seen_producers.size());
	for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
	std::sort(idx.begin(), idx.end(),
		  [&](int a, int b) {
		    return hidra::utils::Producers.at(seen_producers[a]) < hidra::utils::Producers.at(seen_producers[b]); 
		  }
		  );

	// now idx is ordered
	for (int is: idx){
	  auto sub_ev = event.GetSubEvent(is);
	  writeLE(buffer, 45+4*is, static_cast<std::uint32_t>(std::stoul(sub_ev->GetTag(seen_producers[is]+"eventWords"))));
	  appendLE(buffer, DETECTOR_EVENT_MARKER);
	  appendLE(buffer, static_cast<std::uint8_t>(hidra::utils::Producers[seen_producers[is]]));
	  appendLE(buffer, static_cast<std::uint32_t>(sub_ev->GetTriggerN()));
	  appendLE(buffer, SpillNumber); // spill number
	  appendLE(buffer, static_cast<std::uint64_t>(sub_ev->GetTimestampBegin()));
	  appendLE(buffer, static_cast<std::uint64_t>(sub_ev->GetTimestampEnd()));
	  appendLE(buffer, placeholder32);

	  std::vector<uint32_t> block_ids = sub_ev->GetBlockNumList();
	  for (uint32_t ib : block_ids){
	    auto block = sub_ev->GetBlock(ib);
	    buffer.insert(buffer.end(), block.begin(), block.end());
	  }

	  appendLE(buffer, DETECTOR_EVENT_ENDMARKER);
	}



	// Event trailer

	appendLE(buffer, EVENT_TRAILER);
	appendLE(buffer, placeholder8);

	uint32_t EventSize = buffer.size();
	writeLE(buffer, 10, EventSize);

	return buffer;
  }

  void EventSerializer::WriteToFile(const eudaq::Event &event, const std::string &filename){
    
    const auto buffer = Serialize(event);

    std::ofstream output(filename, std::ios::binary);
    if(!output) {
      throw std::runtime_error("Cannot open output file: " + filename);
    }

    output.write(
		 reinterpret_cast<const char *>(buffer.data()),
		 static_cast<std::streamsize>(buffer.size())
		 );
  }

} // namespace hidra
