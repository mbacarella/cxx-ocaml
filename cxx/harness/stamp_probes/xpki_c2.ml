type z = int
module type S = sig type t val x : t end
let mk (type s) (v : s) = (module struct type t = s let x = v end
  : S with type t = s)
let fg (type s) x = let module M = (val x : S with type t = s) in (module M
  : S)
let a = fg (mk 1)
let b = fg (mk "x")
