module W = struct
  module V = struct
    module type S = sig
      include sig type t val v : t module M : sig type q end end
      type z = Zed
    end
    type y = Yed
  end
  type x = Xed
end
type w = Wed
