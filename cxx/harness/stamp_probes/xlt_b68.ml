module F (X : sig type t end) = struct module M = struct module L = struct
  type t = X.t end end end
module N = F (struct type t end)
