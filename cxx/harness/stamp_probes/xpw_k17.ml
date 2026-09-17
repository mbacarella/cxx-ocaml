module type S = sig type t end
let x : (module S with type t = unit) = (module struct type t = unit end)
