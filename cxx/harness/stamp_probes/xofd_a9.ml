module O = struct
  type r = private {a : int}
  module M = struct let v x = x.a end
end
