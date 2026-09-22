module W = struct
  module V = struct
    module type S = sig
      module F : functor (X : sig type t end) -> sig type u end
      type z = Zed
    end
    module M : sig
      module F : functor (X : sig type t end) -> sig type u end
      type z = Zed
    end = struct
      module F (X : sig type t end) = struct type u = X.t end
      type z = Zed
    end
    type y = Yed
  end
  type x = Xed
end
type w = Wed
