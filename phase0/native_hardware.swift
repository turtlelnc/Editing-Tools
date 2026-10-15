import Foundation
import AVFoundation
import VideoToolbox
@main struct Probe {
 static func main() async throws {
 let asset=AVURLAsset(url:URL(fileURLWithPath:CommandLine.arguments[1]))
 let track=try await asset.loadTracks(withMediaType:.video).first!
 let reader=try AVAssetReader(asset:asset)
 let output=AVAssetReaderTrackOutput(track:track,outputSettings:nil);reader.add(output);guard reader.startReading() else {exit(1)}
 guard let sample=output.copyNextSampleBuffer(),let fmt=CMSampleBufferGetFormatDescription(sample) else {print("compressed_sample_unavailable reader_status=\(reader.status.rawValue) error=\(String(describing:reader.error))");exit(2)}
 var session:VTDecompressionSession?
 var callback=VTDecompressionOutputCallbackRecord(decompressionOutputCallback:{ _,_,status,flags,image,pts,duration in
 print("callback_status=\(status) image=\(image != nil) pts=\(CMTimeGetSeconds(pts))")
 },decompressionOutputRefCon:nil)
 let spec=[kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder as String:true] as CFDictionary
 let status=VTDecompressionSessionCreate(allocator:kCFAllocatorDefault,formatDescription:fmt,decoderSpecification:spec,imageBufferAttributes:nil,outputCallback:&callback,decompressionSessionOut:&session)
 print("create_require_hardware=\(status)");guard let s=session,status==0 else {exit(3)}
 var count=0
 func decode(_ sample:CMSampleBuffer){let code=VTDecompressionSessionDecodeFrame(s,sampleBuffer:sample,flags:[],frameRefcon:nil,infoFlagsOut:nil);print("submit=\(code)");if code==0 {count+=1}}
 decode(sample);while let sample=output.copyNextSampleBuffer(){decode(sample)}
 VTDecompressionSessionWaitForAsynchronousFrames(s)
 var value:CFTypeRef?;let prop=VTSessionCopyProperty(s,key:kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,allocator:kCFAllocatorDefault,valueOut:&value)
 print("submitted=\(count) hardware_property_status=\(prop) hardware=\(String(describing:value))")
 VTDecompressionSessionInvalidate(s)
 }
}
