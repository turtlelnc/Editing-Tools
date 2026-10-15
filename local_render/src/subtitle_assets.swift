import AppKit
let dest=CommandLine.arguments[1]
for (name,text) in [("cue1","LOCAL RENDER • cue A"),("cue2","Transition → speed 1.5×")] {
 let rep=NSBitmapImageRep(bitmapDataPlanes:nil,pixelsWide:320,pixelsHigh:180,bitsPerSample:8,samplesPerPixel:4,hasAlpha:true,isPlanar:false,colorSpaceName:.deviceRGB,bytesPerRow:0,bitsPerPixel:0)!
 NSGraphicsContext.saveGraphicsState();NSGraphicsContext.current=NSGraphicsContext(bitmapImageRep:rep)
 NSColor.clear.setFill();NSRect(x:0,y:0,width:320,height:180).fill()
 NSColor.black.withAlphaComponent(0.65).setFill();NSRect(x:10,y:9,width:300,height:30).fill()
 (text as NSString).draw(in:NSRect(x:16,y:14,width:288,height:23),withAttributes:[.font:NSFont.systemFont(ofSize:16,weight:.bold),.foregroundColor:NSColor.white])
 NSGraphicsContext.restoreGraphicsState()
 try rep.representation(using:.png,properties:[:])!.write(to:URL(fileURLWithPath:dest+"/"+name+".png"))
}
