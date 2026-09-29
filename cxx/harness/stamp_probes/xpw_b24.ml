module type S = sig type t end
module M = struct type t = (module S with type t = unit) end
