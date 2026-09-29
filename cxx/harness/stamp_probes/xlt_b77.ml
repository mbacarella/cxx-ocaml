module F (X : sig type t end) = struct module M = struct include X end end
module N : sig end = F (struct type t end)
