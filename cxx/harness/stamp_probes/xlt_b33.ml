module F (X : sig type t end) = struct module M = X module L = X end
module N = F (struct type t end)
