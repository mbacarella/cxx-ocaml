module F (X : sig type t end) = struct module M = struct include X end end
module P : sig end = struct module N = F (struct type t end) end
