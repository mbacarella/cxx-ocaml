open struct module F (X : sig type t val v : int end) = struct type u =
  X.t end end
let z = 1
