module P = struct
  module type A = sig type t end
  module F (X : A) = struct include X class c = object end end
end
