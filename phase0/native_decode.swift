import Foundation
import AVFoundation
import CoreVideo
import CryptoKit
import CoreImage
@main struct Probe {
 static func main() async throws {
 let asset=AVURLAsset(url:URL(fileURLWithPath:CommandLine.arguments[1]))
 let tracks=try await asset.loadTracks(withMediaType:.video)
 guard let track=tracks.first else { fatalError("No video") }
 let transform=try await track.load(.preferredTransform)
 print("preferredTransform=\(transform)")
 let desc=try await track.load(.formatDescriptions)
 print("codec=\(CMFormatDescriptionGetMediaSubType(desc[0]))")
 let reader=try AVAssetReader(asset:asset)
 let output=AVAssetReaderTrackOutput(track:track,outputSettings:[kCVPixelBufferPixelFormatTypeKey as String:kCVPixelFormatType_64RGBAHalf])
 reader.add(output); print("start=\(reader.startReading())")
 var count=0
 while let sample=output.copyNextSampleBuffer() {
 guard let pixel=CMSampleBufferGetImageBuffer(sample) else {fatalError("Missing decoded image")}
 CVPixelBufferLockBaseAddress(pixel,.readOnly)
 let data=Data(bytes:CVPixelBufferGetBaseAddress(pixel)!,count:CVPixelBufferGetBytesPerRow(pixel)*CVPixelBufferGetHeight(pixel))
 print("frame=\(count) width=\(CVPixelBufferGetWidth(pixel)) height=\(CVPixelBufferGetHeight(pixel)) pixel=\(CVPixelBufferGetPixelFormatType(pixel)) hash=\(SHA256.hash(data:data))")
 CVPixelBufferUnlockBaseAddress(pixel,.readOnly)
 if count==0 && CommandLine.arguments.count>2 {
  let context=CIContext();try context.writePNGRepresentation(of:CIImage(cvPixelBuffer:pixel).transformed(by:transform),to:URL(fileURLWithPath:CommandLine.arguments[2]),format:.RGBA8,colorSpace:CGColorSpace(name:CGColorSpace.sRGB)!)
 }
 count+=1
 }
 print("frames=\(count) status=\(reader.status.rawValue) error=\(String(describing:reader.error))")
 if count==0 || reader.status != .completed {exit(1)}
 }
}
