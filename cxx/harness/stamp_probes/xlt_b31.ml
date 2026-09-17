module F (X : sig type t end) = struct module M = X end
module P = struct module N = F (struct type t end) end
