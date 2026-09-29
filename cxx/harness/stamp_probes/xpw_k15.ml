module type S = sig type t end
module type T = sig type t = (module S with type t = unit) end
