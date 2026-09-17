module P = struct
  module F (X : sig end) = struct class c = object end class d = object end end
end
