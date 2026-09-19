type z = int
module type S = sig type t val x : t end
let mk (type s) (v : s) = (module struct type t = s let x = v end
  : S with type t = s)
