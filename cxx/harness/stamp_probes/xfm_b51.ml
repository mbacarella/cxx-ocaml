let r = ref 0;;
let f (x : Marshal.extern_flags) = let module M = struct let y = x end in M.y
