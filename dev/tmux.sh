#!/bin/bash
# Window and pane numbers start from 1.

SESSION="file-sync"
SESSIONEXISTS=$(tmux ls | grep $SESSION)

if [ "$SESSIONEXISTS" = "" ]; then

  tmux new-session -d -s $SESSION

  tmux rename-window -t 1 "Writing"
  tmux send-keys -t "$SESSION:Writing.1" "nvim ." Enter

  tmux new-window -t $SESSION -n 'Transfer'
  tmux split-window -h -t "$SESSION:Transfer"
  tmux send-keys -t "$SESSION:Transfer.1" "cd $PROJECTS/file-sync/" Enter "clear" Enter

  tmux new-window -t $SESSION -n 'Testing'
  tmux split-window -v -t "$SESSION:Testing"
  tmux split-window -h -t "$SESSION:Testing.1"
  tmux split-window -h -t "$SESSION:Testing.1"
  tmux split-window -h -t "$SESSION:Testing.2"
  tmux split-window -h -t "$SESSION:Testing.2"
  tmux select-layout -t "$SESSION:Testing" tiled

fi

tmux attach-session -t "$SESSION:Writing"
