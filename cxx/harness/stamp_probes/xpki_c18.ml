type z = int
module type S = sig type t val x : t end
let mk (type s) (v : s) = (module struct type t = s let x = v end
  : S with type t = s)
let mk2 v = mk v
let a = [mk2 1; mk2 2]
