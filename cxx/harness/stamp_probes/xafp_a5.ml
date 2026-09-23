module M : sig val p : int * int end = struct let p = (1, 2) end
let q = M.p
