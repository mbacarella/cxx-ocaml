let r = ref 0;;
let _ = Marshal.to_string (object end) [Marshal.Closures];;
