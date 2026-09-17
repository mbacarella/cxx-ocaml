module type ORD = sig type t end
module type SET = sig type t end
module B (F : sig end) = struct
  module rec Elt : sig type t end = struct type t = int end
end
