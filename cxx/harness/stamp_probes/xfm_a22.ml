let r = ref 0;;
let _ = Marshal.to_string 0 (let x = 1 in ignore x; [Marshal.Closures])
