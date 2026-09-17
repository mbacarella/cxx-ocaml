let r = ref 0;;
let _ = Marshal.to_string 0 (if true then [] else [Marshal.Closures])
