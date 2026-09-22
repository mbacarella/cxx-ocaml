module W = struct
  module type S = sig
    include sig type t val v : t end
    type z = Zed
  end
  type y = Yed
end
type x = Xed
