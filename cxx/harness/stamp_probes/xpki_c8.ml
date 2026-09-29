type z = int
module type S = sig type t val x : t end
let mk (type s) (v : s) = (module struct type t = s let x = v end
  : S with type t = s)
let fg (type s) x = let module M = (val x : S with type t = s) in (module M
  : S)
let a = List.map fg [mk 1; mk 2]
