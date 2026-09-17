module F (X : sig type t end) = struct module M = struct type t end end
module N = F (struct type t end)
