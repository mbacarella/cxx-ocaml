module type ORD = sig type t end
module type SET = sig type t end
module N = struct module M = struct
  module B (F : functor (X : ORD) -> SET) = struct end
end end
