module F (X : sig type t end) = struct module M = X end
module N = F (struct type t = int end)
