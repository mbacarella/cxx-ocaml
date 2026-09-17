let r = ref 0;;
let f (x : Marshal.extern_flags) = try x with _ -> Marshal.Closures
