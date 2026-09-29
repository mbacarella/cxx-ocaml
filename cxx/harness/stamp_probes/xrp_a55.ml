module P = struct
  module F (X : sig end) : sig class c : object end end =
    struct class c = object end end
end
