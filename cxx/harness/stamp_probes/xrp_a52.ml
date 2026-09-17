module P = struct
  module type A = sig type t end
  module F (X : A) = struct
    class ['a] c (l : 'a list) = object method m = l end
  end
end
