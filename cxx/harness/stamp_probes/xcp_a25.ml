module W = struct
  module V = struct
  module M : sig
    module F : functor (_ : sig type t end) -> sig type u end
    type z = Zed
  end = struct
    module F (_ : sig type t end) = struct type u = int end
    type z = Zed
  end
  type y = Yed
  end
  type x = Xed
end
type w = Wed
