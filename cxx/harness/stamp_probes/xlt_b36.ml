module F (X : sig type t end) = struct module M : sig type t = X.t end = X end
module N = F (struct type t end)
