module P = struct
  module type MyT = sig type t module M : sig type u end end
  module MyMap(X : MyT) = struct include X end
end
