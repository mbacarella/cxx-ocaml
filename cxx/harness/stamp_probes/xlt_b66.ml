module F (X : sig type t end) = struct module M = struct module L = X end end
module N = F (struct type t end)
