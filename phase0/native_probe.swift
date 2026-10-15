import Foundation
import VideoToolbox
import CoreMedia
import AVFoundation
for (name, codec) in [("ProRes422", kCMVideoCodecType_AppleProRes422), ("ProResRAW", kCMVideoCodecType_AppleProResRAW), ("ProResRAWHQ", kCMVideoCodecType_AppleProResRAWHQ)] {
 var fmt: CMVideoFormatDescription?
 let fs = CMVideoFormatDescriptionCreate(allocator: kCFAllocatorDefault, codecType: codec, width: 1920, height: 1080, extensions: nil, formatDescriptionOut: &fmt)
 var session: VTDecompressionSession?
 let status = fmt.map { VTDecompressionSessionCreate(allocator: kCFAllocatorDefault, formatDescription: $0, decoderSpecification: nil, imageBufferAttributes: nil, outputCallback: nil, decompressionSessionOut: &session) } ?? fs
 print("\(name) format=\(fs) session=\(status) hardware_supported=\(VTIsHardwareDecodeSupported(codec))")
 if let s = session {
 var value: CFTypeRef?
 let p = VTSessionCopyProperty(s, key: kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, allocator: kCFAllocatorDefault, valueOut: &value)
 print("hardware_property=\(p) value=\(String(describing:value))")
 VTDecompressionSessionInvalidate(s)
 }
}
