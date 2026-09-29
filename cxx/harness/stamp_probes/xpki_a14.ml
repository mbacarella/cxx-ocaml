type z = int
module type S = sig type t val x : t end
let mk (v : int) = (module struct type t = int let x = v end : S)
let x = [mk 1; mk 2]
