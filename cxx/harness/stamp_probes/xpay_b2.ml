module M : sig val x : int [@@deprecated "v"] end = struct let x = 1 end
