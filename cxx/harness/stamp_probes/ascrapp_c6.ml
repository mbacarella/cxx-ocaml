module M : sig val y : int end = struct include Set.Make(String) let y = 0 end
