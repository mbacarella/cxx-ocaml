module W = struct
  module V = struct
    module type S = sig
      class c : object method m : int end
      class type ct = object method m : int end
      type z = Zed
    end
    type y = Yed
  end
  type x = Xed
end
type w = Wed
