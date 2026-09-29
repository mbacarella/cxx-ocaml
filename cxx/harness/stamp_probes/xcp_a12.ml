module W = struct
  module type S = sig
    module F : functor (X : sig type t end) -> sig type u end
    type z = Zed
  end
  type y = Yed
end
type x = Xed
